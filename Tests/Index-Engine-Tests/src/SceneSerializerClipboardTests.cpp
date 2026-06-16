#include <doctest/doctest.h>

#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/PrefabInstanceComponent.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/General/UUIDComponent.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializer.hpp"

#include <string>

using namespace Index;

TEST_CASE("Clipboard entity serialization preserves child hierarchy")
{
	auto scene = Scene::CreateDetachedScene("Clipboard Test");

	Entity root = scene->CreateEntity("Root");
	auto& rootTransform = root.GetComponent<Transform2DComponent>();
	rootTransform.LocalPosition = { 2.0f, 3.0f };
	rootTransform.Position = rootTransform.LocalPosition;

	Entity child = scene->CreateEntity("Child");
	auto& childTransform = child.GetComponent<Transform2DComponent>();
	childTransform.LocalPosition = { -5.0f, 0.0f };
	childTransform.Position = childTransform.LocalPosition;
	child.SetParent(root);

	const uint64_t originalRootId = static_cast<uint64_t>(scene->GetComponent<UUIDComponent>(root.GetHandle()).Id);
	const uint64_t originalChildId = static_cast<uint64_t>(scene->GetComponent<UUIDComponent>(child.GetHandle()).Id);

	Json::Value clipboardValue = SceneSerializer::SerializeEntityForClipboard(*scene, root.GetHandle());
	REQUIRE(clipboardValue.IsObject());

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, clipboardValue);
	REQUIRE(cloneHandle != entt::null);
	REQUIRE(scene->IsValid(cloneHandle));

	Entity clone = scene->GetEntity(cloneHandle);
	CHECK(clone.GetName() == "Root");
	CHECK(static_cast<uint64_t>(scene->GetComponent<UUIDComponent>(cloneHandle).Id) != originalRootId);

	const auto& clonedChildren = clone.GetChildren();
	REQUIRE(clonedChildren.size() == 1);

	Entity clonedChild = scene->GetEntity(clonedChildren.front());
	CHECK(clonedChild.GetName() == "Child");
	CHECK(clonedChild.GetParent().GetHandle() == cloneHandle);
	CHECK(static_cast<uint64_t>(scene->GetComponent<UUIDComponent>(clonedChild.GetHandle()).Id) != originalChildId);

	const auto& clonedChildTransform = clonedChild.GetComponent<Transform2DComponent>();
	CHECK(clonedChildTransform.LocalPosition.x == doctest::Approx(-5.0f));
	CHECK(clonedChildTransform.LocalPosition.y == doctest::Approx(0.0f));
}

TEST_CASE("Clipboard entity serialization preserves grandchild hierarchy")
{
	auto scene = Scene::CreateDetachedScene("Clipboard Deep Test");

	Entity root = scene->CreateEntity("Root");
	Entity child = scene->CreateEntity("Child");
	Entity grandchild = scene->CreateEntity("Grandchild");
	Entity grandchild2 = scene->CreateEntity("Grandchild2");
	child.SetParent(root);
	grandchild.SetParent(child);
	grandchild2.SetParent(child);

	Json::Value clipboardValue = SceneSerializer::SerializeEntityForClipboard(*scene, root.GetHandle());
	REQUIRE(clipboardValue.IsObject());

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, clipboardValue);
	REQUIRE(cloneHandle != entt::null);

	Entity clone = scene->GetEntity(cloneHandle);
	REQUIRE(clone.GetChildren().size() == 1);

	Entity clonedChild = scene->GetEntity(clone.GetChildren().front());
	CHECK(clonedChild.GetName() == "Child");
	REQUIRE(clonedChild.GetChildren().size() == 2);

	Entity clonedGrandchild = scene->GetEntity(clonedChild.GetChildren()[0]);
	Entity clonedGrandchild2 = scene->GetEntity(clonedChild.GetChildren()[1]);
	CHECK(clonedGrandchild.GetParent().GetHandle() == clonedChild.GetHandle());
	CHECK(clonedGrandchild2.GetParent().GetHandle() == clonedChild.GetHandle());
	CHECK((clonedGrandchild.GetName() == "Grandchild" || clonedGrandchild.GetName() == "Grandchild2"));
}

TEST_CASE("Editor-style duplicate (re-parenting under source's parent) preserves children")
{
	auto scene = Scene::CreateDetachedScene("Editor Duplicate Test");

	Entity grandparent = scene->CreateEntity("Grandparent");
	Entity parent = scene->CreateEntity("Parent");
	Entity child = scene->CreateEntity("Child");
	parent.SetParent(grandparent);
	child.SetParent(parent);

	Json::Value clipboardValue = SceneSerializer::SerializeEntityForClipboard(*scene, parent.GetHandle());
	REQUIRE(clipboardValue.IsObject());

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, clipboardValue);
	REQUIRE(cloneHandle != entt::null);

	Entity clone = scene->GetEntity(cloneHandle);
	clone.SetParent(grandparent);

	CHECK(clone.GetParent().GetHandle() == grandparent.GetHandle());
	REQUIRE(clone.GetChildren().size() == 1);

	Entity clonedChild = scene->GetEntity(clone.GetChildren().front());
	CHECK(clonedChild.GetName() == "Child");
	CHECK(clonedChild.GetParent().GetHandle() == cloneHandle);
}

TEST_CASE("Duplicating a prefab instance preserves prefab origin and PrefabGUID")
{
	auto scene = Scene::CreateDetachedScene("Prefab Duplicate Test");

	Entity source = scene->CreateEntity("PrefabRoot");
	Entity sourceChild = scene->CreateEntity("PrefabChild");
	sourceChild.SetParent(source);

	constexpr uint64_t k_PrefabGuid = 0x1234ABCDull;
	constexpr uint64_t k_RootSourceEntityId = 0xAAAA1ull;
	constexpr uint64_t k_ChildSourceEntityId = 0xBBBB2ull;

	scene->SetEntityMetaData(source.GetHandle(), EntityOrigin::Prefab, AssetGUID(k_PrefabGuid));
	scene->SetEntityMetaData(sourceChild.GetHandle(), EntityOrigin::Prefab, AssetGUID(k_PrefabGuid));
	REQUIRE(scene->HasComponent<PrefabInstanceComponent>(source.GetHandle()));
	scene->GetComponent<PrefabInstanceComponent>(source.GetHandle()).SourceEntityId = k_RootSourceEntityId;
	scene->GetComponent<PrefabInstanceComponent>(sourceChild.GetHandle()).SourceEntityId = k_ChildSourceEntityId;

	Json::Value clipboardValue = SceneSerializer::SerializeEntityForClipboard(*scene, source.GetHandle());
	REQUIRE(clipboardValue.IsObject());

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, clipboardValue);
	REQUIRE(cloneHandle != entt::null);

	CHECK(scene->GetEntityOrigin(cloneHandle) == EntityOrigin::Prefab);
	CHECK(static_cast<uint64_t>(scene->GetPrefabGUID(cloneHandle)) == k_PrefabGuid);
	REQUIRE(scene->HasComponent<PrefabInstanceComponent>(cloneHandle));
	CHECK(scene->GetComponent<PrefabInstanceComponent>(cloneHandle).SourceEntityId == k_RootSourceEntityId);

	Entity clone = scene->GetEntity(cloneHandle);
	REQUIRE(clone.GetChildren().size() == 1);
	const EntityHandle cloneChildHandle = clone.GetChildren().front();
	CHECK(scene->GetEntityOrigin(cloneChildHandle) == EntityOrigin::Prefab);
	CHECK(static_cast<uint64_t>(scene->GetPrefabGUID(cloneChildHandle)) == k_PrefabGuid);
	REQUIRE(scene->HasComponent<PrefabInstanceComponent>(cloneChildHandle));
	CHECK(scene->GetComponent<PrefabInstanceComponent>(cloneChildHandle).SourceEntityId == k_ChildSourceEntityId);
}

TEST_CASE("Entity-ref remap retargets internal refs and passes external refs through")
{
	auto scene = Scene::CreateDetachedScene("Entity Ref Remap Test");

	Entity original = scene->CreateEntity("Original");
	Entity clone = scene->CreateEntity("Clone");
	const uint64_t originalId = scene->GetEntityPersistentID(original.GetHandle());
	const uint64_t cloneId = scene->GetEntityPersistentID(clone.GetHandle());
	REQUIRE(originalId != 0);
	REQUIRE(cloneId != 0);
	REQUIRE(originalId != cloneId);

	CHECK_FALSE(scene->IsEntityRefRemapActive());

	{
		Scene::EntityRefRemapScope scope(*scene);
		CHECK(scene->IsEntityRefRemapActive());
		scene->RegisterEntityRefRemap(originalId, cloneId);

		// Internal ref: an id that pointed at the original now resolves to the clone.
		CHECK(scene->TranslateEntityRef(originalId) == cloneId);
		EntityHandle resolved = entt::null;
		REQUIRE(scene->TryResolveEntityRef(originalId, resolved));
		CHECK(resolved == clone.GetHandle());

		// External ref: an id absent from the map is unchanged and resolves to itself.
		CHECK(scene->TranslateEntityRef(cloneId) == cloneId);
		EntityHandle external = entt::null;
		REQUIRE(scene->TryResolveEntityRef(cloneId, external));
		CHECK(external == clone.GetHandle());
	}

	// Scope closed: the map is cleared and the original id resolves to the original again.
	CHECK_FALSE(scene->IsEntityRefRemapActive());
	CHECK(scene->TranslateEntityRef(originalId) == originalId);
	EntityHandle afterScope = entt::null;
	REQUIRE(scene->TryResolveEntityRef(originalId, afterScope));
	CHECK(afterScope == original.GetHandle());
}

TEST_CASE("Duplicate remaps script entity-ref fields to the cloned child")
{
	auto scene = Scene::CreateDetachedScene("Script Ref Remap Test");

	// A live entity OUTSIDE the duplicated subtree; refs to it must survive unchanged.
	Entity outside = scene->CreateEntity("Outside");
	const uint64_t outsideId = scene->GetEntityPersistentID(outside.GetHandle());
	REQUIRE(outsideId != 0);

	// The "original" persistent ids the clipboard's entity refs were serialized against.
	constexpr uint64_t k_RootSrcId = 0xA11CEull;
	constexpr uint64_t k_ChildSrcId = 0xB0Bull;

	using Json::Value;
	auto makeEntity = [](const char* clipId, uint64_t srcId, const char* name, const char* parentClipId) {
		Value e = Value::MakeObject();
		e.AddMember("uuid", Value(std::string(clipId)));
		e.AddMember("sourceUuid", Value(std::to_string(srcId)));
		e.AddMember("name", Value(std::string(name)));
		Value t = Value::MakeObject();
		t.AddMember("posX", Value(0.0f));
		t.AddMember("posY", Value(0.0f));
		t.AddMember("rotation", Value(0.0f));
		t.AddMember("scaleX", Value(1.0f));
		t.AddMember("scaleY", Value(1.0f));
		e.AddMember("Transform2D", std::move(t));
		if (parentClipId) {
			e.AddMember("parentUuid", Value(std::string(parentClipId)));
		}
		return e;
	};

	Value root = makeEntity("1", k_RootSrcId, "ScriptRoot", nullptr);

	Value scripts = Value::MakeArray();
	{
		Value s = Value::MakeObject();
		s.AddMember("className", Value(std::string("TestScript")));
		s.AddMember("type", Value(std::string("Managed")));
		scripts.Append(std::move(s));
	}
	root.AddMember("Scripts", std::move(scripts));

	auto addField = [](Value& arr, const char* fieldName, const char* type, const std::string& val) {
		Value f = Value::MakeObject();
		f.AddMember("name", Value(std::string(fieldName)));
		f.AddMember("type", Value(std::string(type)));
		f.AddMember("value", Value(val));
		arr.Append(std::move(f));
	};

	Value fields = Value::MakeArray();
	addField(fields, "Target",    "entity", std::to_string(k_ChildSrcId));               // internal -> remap
	addField(fields, "External",  "entity", std::to_string(outsideId));                  // external -> unchanged
	addField(fields, "CompRef",   "Health", std::to_string(k_ChildSrcId) + ":Health");   // component ref -> remap leading id
	addField(fields, "PrefabRef", "entity", "prefab:9999");                              // asset ref -> never remapped

	Value fieldsByClass = Value::MakeObject();
	fieldsByClass.AddMember("TestScript", std::move(fields));
	root.AddMember("ScriptFields", std::move(fieldsByClass));

	Value child = makeEntity("2", k_ChildSrcId, "ScriptChild", "1");

	Value entities = Value::MakeArray();
	entities.Append(Value(root));
	entities.Append(std::move(child));

	Value clipboard = Value::MakeObject();
	clipboard.AddMember("ClipboardEntity", Value(true));
	clipboard.AddMember("Entity", std::move(root));
	clipboard.AddMember("Entities", std::move(entities));

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, clipboard);
	REQUIRE(cloneHandle != entt::null);

	Entity clone = scene->GetEntity(cloneHandle);
	REQUIRE(clone.GetChildren().size() == 1);
	const EntityHandle cloneChildHandle = clone.GetChildren().front();
	const uint64_t cloneChildId = scene->GetEntityPersistentID(cloneChildHandle);
	REQUIRE(cloneChildId != 0);
	REQUIRE(cloneChildId != k_ChildSrcId);

	REQUIRE(scene->HasComponent<ScriptComponent>(cloneHandle));
	auto& sc = scene->GetComponent<ScriptComponent>(cloneHandle);

	// Internal subtree refs retarget to the clone's own child.
	CHECK(sc.PendingFieldValues["TestScript.Target"] == std::to_string(cloneChildId));
	CHECK(sc.PendingFieldValues["TestScript.CompRef"] == std::to_string(cloneChildId) + ":Health");
	// External + asset refs are left exactly as they were.
	CHECK(sc.PendingFieldValues["TestScript.External"] == std::to_string(outsideId));
	CHECK(sc.PendingFieldValues["TestScript.PrefabRef"] == "prefab:9999");
}
