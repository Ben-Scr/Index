// Tests for the editor undo/redo core: UndoStack ordering / redo-invalidation /
// depth cap / merge, and TransformEditCommand restore-by-persistent-id. No ImGui
// or GPU — the gizmo wiring that feeds these is exercised manually.

#include <doctest/doctest.h>

#include "Undo/UndoStack.hpp"
#include "Undo/TransformEditCommand.hpp"
#include "Undo/InspectorEditCommand.hpp"

#include "Scene/Scene.hpp"
#include "Scene/Entity.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Inspector/PropertyValue.hpp"
#include "Inspector/PropertyType.hpp"

using namespace Index;

namespace {
	// Records how many times it was undone/redone so stack ordering and the
	// depth cap can be asserted without inspecting UndoStack internals.
	struct CountingCommand : public EditCommand {
		int* UndoCount = nullptr;
		int* RedoCount = nullptr;
		bool Mergeable = false;

		void Undo(Scene&) override { if (UndoCount) ++*UndoCount; }
		void Redo(Scene&) override { if (RedoCount) ++*RedoCount; }
		bool TryMergeWith(const EditCommand&) override { return Mergeable; }
	};

	std::unique_ptr<CountingCommand> MakeCounting(int* undo, int* redo, bool mergeable = false) {
		auto cmd = std::make_unique<CountingCommand>();
		cmd->UndoCount = undo;
		cmd->RedoCount = redo;
		cmd->Mergeable = mergeable;
		return cmd;
	}

	TransformSnapshot FlatSnapshot(const Vec2& pos) {
		// Root entity, so world == local; lets Propagate (which re-derives world
		// from local) leave the asserted world Position untouched.
		TransformSnapshot s;
		s.Position = pos;
		s.LocalPosition = pos;
		return s;
	}
}

TEST_CASE("UndoStack: empty stack reports nothing and Undo/Redo are no-ops") {
	auto scene = Scene::CreateDetachedScene("Undo Empty");
	UndoStack stack;

	CHECK_FALSE(stack.CanUndo());
	CHECK_FALSE(stack.CanRedo());
	CHECK_NOTHROW(stack.Undo(*scene));
	CHECK_NOTHROW(stack.Redo(*scene));
}

TEST_CASE("UndoStack: push/undo/redo invoke the command and flip the flags") {
	auto scene = Scene::CreateDetachedScene("Undo Roundtrip");
	UndoStack stack;
	int undos = 0, redos = 0;

	stack.Push(MakeCounting(&undos, &redos));
	CHECK(stack.CanUndo());
	CHECK_FALSE(stack.CanRedo());

	stack.Undo(*scene);
	CHECK(undos == 1);
	CHECK_FALSE(stack.CanUndo());
	CHECK(stack.CanRedo());

	stack.Redo(*scene);
	CHECK(redos == 1);
	CHECK(stack.CanUndo());
	CHECK_FALSE(stack.CanRedo());
}

TEST_CASE("UndoStack: a fresh push invalidates the redo branch") {
	auto scene = Scene::CreateDetachedScene("Undo RedoClear");
	UndoStack stack;
	int undos = 0, redos = 0;

	stack.Push(MakeCounting(&undos, &redos));
	stack.Undo(*scene);
	REQUIRE(stack.CanRedo());

	stack.Push(MakeCounting(&undos, &redos));
	CHECK_FALSE(stack.CanRedo());
}

TEST_CASE("UndoStack: a null command is ignored") {
	UndoStack stack;
	stack.Push(nullptr);
	CHECK_FALSE(stack.CanUndo());
}

TEST_CASE("UndoStack: mergeable top folds in the next push (single entry)") {
	auto scene = Scene::CreateDetachedScene("Undo Merge");
	UndoStack stack;
	int undos = 0, redos = 0;

	// First command reports mergeable, so the second is folded into it.
	stack.Push(MakeCounting(&undos, &redos, /*mergeable*/ true));
	stack.Push(MakeCounting(&undos, &redos));

	stack.Undo(*scene);
	CHECK(undos == 1);            // only one entry existed
	CHECK_FALSE(stack.CanUndo()); // ...and it's now consumed
}

TEST_CASE("UndoStack: oldest entries drop past the depth cap") {
	auto scene = Scene::CreateDetachedScene("Undo Cap");
	UndoStack stack;
	int undos = 0, redos = 0;

	// Mirrors UndoStack::k_MaxDepth. Push beyond it; only the cap's worth of
	// most-recent commands should remain undoable.
	constexpr int kMaxDepth = 256;
	for (int i = 0; i < kMaxDepth + 10; ++i) {
		stack.Push(MakeCounting(&undos, &redos));
	}

	int undone = 0;
	while (stack.CanUndo()) {
		stack.Undo(*scene);
		++undone;
	}
	CHECK(undone == kMaxDepth);
	CHECK(undos == kMaxDepth);
}

TEST_CASE("TransformEditCommand::HasChange is false only when nothing moved") {
	std::vector<TransformEditCommand::Entry> noMove = { { 1, FlatSnapshot({ 2.0f, 3.0f }), FlatSnapshot({ 2.0f, 3.0f }) } };
	std::vector<TransformEditCommand::Entry> moved  = { { 1, FlatSnapshot({ 2.0f, 3.0f }), FlatSnapshot({ 9.0f, 3.0f }) } };

	CHECK_FALSE(TransformEditCommand(std::move(noMove)).HasChange());
	CHECK(TransformEditCommand(std::move(moved)).HasChange());
}

TEST_CASE("TransformEditCommand: Undo restores Before, Redo restores After") {
	auto scene = Scene::CreateDetachedScene("Transform Undo");
	Entity e = scene->CreateEntity("Mover");
	const uint64_t id = scene->GetEntityPersistentID(e.GetHandle());
	REQUIRE(id != 0);

	const Vec2 before{ 1.0f, 2.0f };
	const Vec2 after{ 7.0f, -4.0f };

	std::vector<TransformEditCommand::Entry> entries = { { id, FlatSnapshot(before), FlatSnapshot(after) } };
	TransformEditCommand command(std::move(entries));

	command.Undo(*scene);
	auto& tx = e.GetComponent<Transform2DComponent>();
	CHECK(tx.Position.x == doctest::Approx(before.x));
	CHECK(tx.Position.y == doctest::Approx(before.y));

	command.Redo(*scene);
	CHECK(tx.Position.x == doctest::Approx(after.x));
	CHECK(tx.Position.y == doctest::Approx(after.y));
}

TEST_CASE("TransformEditCommand: each entity is restored to its own snapshot") {
	auto scene = Scene::CreateDetachedScene("Transform Multi");
	Entity a = scene->CreateEntity("A");
	Entity b = scene->CreateEntity("B");
	const uint64_t idA = scene->GetEntityPersistentID(a.GetHandle());
	const uint64_t idB = scene->GetEntityPersistentID(b.GetHandle());

	std::vector<TransformEditCommand::Entry> entries = {
		{ idA, FlatSnapshot({ 10.0f, 0.0f }), FlatSnapshot({ 11.0f, 0.0f }) },
		{ idB, FlatSnapshot({ 20.0f, 0.0f }), FlatSnapshot({ 22.0f, 0.0f }) },
	};
	TransformEditCommand command(std::move(entries));

	command.Undo(*scene);
	CHECK(a.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(10.0f));
	CHECK(b.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(20.0f));

	command.Redo(*scene);
	CHECK(a.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(11.0f));
	CHECK(b.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(22.0f));
}

TEST_CASE("TransformEditCommand: an unresolvable id is skipped, not fatal") {
	auto scene = Scene::CreateDetachedScene("Transform Stale");
	Entity e = scene->CreateEntity("Live");
	const uint64_t liveId = scene->GetEntityPersistentID(e.GetHandle());
	const uint64_t deadId = liveId ^ 0xABCDEFu; // not present in the scene

	std::vector<TransformEditCommand::Entry> entries = {
		{ deadId, FlatSnapshot({ 0.0f, 0.0f }), FlatSnapshot({ 0.0f, 0.0f }) },
		{ liveId, FlatSnapshot({ 5.0f, 5.0f }), FlatSnapshot({ 6.0f, 6.0f }) },
	};
	TransformEditCommand command(std::move(entries));

	CHECK_NOTHROW(command.Undo(*scene));
	CHECK(e.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(5.0f));
}

namespace {
	// Mirrors how a real descriptor's Set writes a scalar field; the command
	// replays exactly this through stored before/after PropertyValues.
	InspectorEditCommand::ApplyFn PositionXSetter() {
		return [](Entity& e, const PropertyValue& v) {
			e.GetComponent<Transform2DComponent>().Position.x = static_cast<float>(v.FloatValue);
		};
	}

	PropertyValue FloatValue(double x) {
		PropertyValue v;
		v.Type = PropertyType::Float;
		v.FloatValue = x;
		return v;
	}
}

TEST_CASE("InspectorEditCommand::HasChange is false only when before == after") {
	std::vector<InspectorEditCommand::Entry> same = { { 1, FloatValue(2.0), FloatValue(2.0) } };
	std::vector<InspectorEditCommand::Entry> diff = { { 1, FloatValue(2.0), FloatValue(5.0) } };

	CHECK_FALSE(InspectorEditCommand(std::move(same), PositionXSetter(), "x").HasChange());
	CHECK(InspectorEditCommand(std::move(diff), PositionXSetter(), "x").HasChange());
}

TEST_CASE("InspectorEditCommand replays the descriptor Set for Undo/Redo") {
	auto scene = Scene::CreateDetachedScene("Inspector Undo");
	Entity e = scene->CreateEntity("Field");
	const uint64_t id = scene->GetEntityPersistentID(e.GetHandle());
	REQUIRE(id != 0);

	std::vector<InspectorEditCommand::Entry> entries = { { id, FloatValue(1.0), FloatValue(9.0) } };
	InspectorEditCommand command(std::move(entries), PositionXSetter(), "Position.x");

	command.Undo(*scene);
	CHECK(e.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(1.0f));

	command.Redo(*scene);
	CHECK(e.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(9.0f));
}

TEST_CASE("InspectorEditCommand restores per-entity values across a multi-select edit") {
	auto scene = Scene::CreateDetachedScene("Inspector Multi");
	Entity a = scene->CreateEntity("A");
	Entity b = scene->CreateEntity("B");

	std::vector<InspectorEditCommand::Entry> entries = {
		{ scene->GetEntityPersistentID(a.GetHandle()), FloatValue(1.0), FloatValue(3.0) },
		{ scene->GetEntityPersistentID(b.GetHandle()), FloatValue(2.0), FloatValue(4.0) },
	};
	InspectorEditCommand command(std::move(entries), PositionXSetter(), "Position.x");

	command.Undo(*scene);
	CHECK(a.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(1.0f));
	CHECK(b.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(2.0f));

	command.Redo(*scene);
	CHECK(a.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(3.0f));
	CHECK(b.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(4.0f));
}

TEST_CASE("InspectorEditCommand skips an unresolvable id without touching live entities") {
	auto scene = Scene::CreateDetachedScene("Inspector Stale");
	Entity e = scene->CreateEntity("Live");
	const uint64_t liveId = scene->GetEntityPersistentID(e.GetHandle());
	const uint64_t deadId = liveId ^ 0x1234u;

	std::vector<InspectorEditCommand::Entry> entries = {
		{ deadId, FloatValue(0.0), FloatValue(0.0) },
		{ liveId, FloatValue(7.0), FloatValue(8.0) },
	};
	InspectorEditCommand command(std::move(entries), PositionXSetter(), "Position.x");

	CHECK_NOTHROW(command.Undo(*scene));
	CHECK(e.GetComponent<Transform2DComponent>().Position.x == doctest::Approx(7.0f));
}

TEST_CASE("Command labels supply the toast text") {
	std::vector<TransformEditCommand::Entry> t = { { 1, FlatSnapshot({ 0.0f, 0.0f }), FlatSnapshot({ 1.0f, 0.0f }) } };
	CHECK(TransformEditCommand(std::move(t), "Move").Label() == "Move");

	std::vector<InspectorEditCommand::Entry> named = { { 1, FloatValue(0.0), FloatValue(1.0) } };
	CHECK(InspectorEditCommand(std::move(named), PositionXSetter(), "Position").Label() == "Edit Position");

	std::vector<InspectorEditCommand::Entry> unnamed = { { 1, FloatValue(0.0), FloatValue(1.0) } };
	CHECK(InspectorEditCommand(std::move(unnamed), PositionXSetter(), "").Label() == "Edit Property");
}

TEST_CASE("UndoStack::Undo/Redo return the affected command's label") {
	auto scene = Scene::CreateDetachedScene("Label Stack");
	Entity e = scene->CreateEntity("E");
	const uint64_t id = scene->GetEntityPersistentID(e.GetHandle());

	UndoStack stack;
	std::vector<TransformEditCommand::Entry> entries = { { id, FlatSnapshot({ 0.0f, 0.0f }), FlatSnapshot({ 5.0f, 0.0f }) } };
	stack.Push(std::make_unique<TransformEditCommand>(std::move(entries), "Move"));

	CHECK(stack.Undo(*scene) == "Move");
	CHECK(stack.Redo(*scene) == "Move");
	CHECK(stack.Undo(*scene) == "Move");
	CHECK(stack.Undo(*scene).empty()); // nothing left
}
