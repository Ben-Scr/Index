#pragma once
#include "Undo/EditCommand.hpp"
#include "Collections/Vec2.hpp"
#include "Scene/EntityHandle.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Index {

	class Scene;

	// Full Transform2DComponent state. Both world and local are kept: the gizmo
	// writes both, and restoring only one would desync them until the next
	// hierarchy pass.
	struct TransformSnapshot {
		Vec2 Position{ 0.0f, 0.0f };
		Vec2 Scale{ 1.0f, 1.0f };
		float Rotation = 0.0f;
		Vec2 LocalPosition{ 0.0f, 0.0f };
		Vec2 LocalScale{ 1.0f, 1.0f };
		float LocalRotation = 0.0f;
	};

	// Authored RectTransform2DComponent state. The resolved/world fields
	// (Rotation/Scale/ResolvedMin/Max) are re-derived by UILayoutSystem every
	// frame, so they aren't stored.
	struct RectTransformSnapshot {
		Vec2 AnchorMin{ 0.5f, 0.5f };
		Vec2 AnchorMax{ 0.5f, 0.5f };
		Vec2 Pivot{ 0.5f, 0.5f };
		Vec2 AnchoredPosition{ 0.0f, 0.0f };
		Vec2 SizeDelta{ 100.0f, 100.0f };
		float LocalRotation = 0.0f;
		Vec2 LocalScale{ 1.0f, 1.0f };
	};

	// Snapshot of whichever transform an entity carries (the two are mutually
	// exclusive). Valid is false for entities with neither.
	struct EntityTransformSnapshot {
		bool Valid = false;
		bool IsRect = false;
		TransformSnapshot Transform{};
		RectTransformSnapshot Rect{};

		EntityTransformSnapshot() = default;
		// Implicit from a bare Transform2D snapshot so existing call sites (and
		// tests) that deal in TransformSnapshot keep compiling unchanged.
		EntityTransformSnapshot(const TransformSnapshot& t)
			: Valid(true), IsRect(false), Transform(t) {}
		EntityTransformSnapshot(const RectTransformSnapshot& r)
			: Valid(true), IsRect(true), Rect(r) {}
	};

	// Capture / restore the authored transform of an entity (Transform2D or
	// RectTransform2D). Shared by gizmo + resize drag recording and undo restore.
	EntityTransformSnapshot CaptureEntityTransform(Scene& scene, EntityHandle handle);
	void WriteEntityTransform(Scene& scene, EntityHandle handle, const EntityTransformSnapshot& snap);
	bool EntityTransformsDiffer(const EntityTransformSnapshot& a, const EntityTransformSnapshot& b);

	// One undo step for a gizmo/resize drag spanning one or more selected entities.
	class TransformEditCommand : public EditCommand {
	public:
		struct Entry {
			uint64_t PersistentId = 0;
			EntityTransformSnapshot Before;
			EntityTransformSnapshot After;
		};

		explicit TransformEditCommand(std::vector<Entry> entries, std::string label = "Transform")
			: m_Entries(std::move(entries)), m_Label(std::move(label)) {}

		// False when nothing actually moved, so a click-without-drag never pushes
		// an empty step.
		bool HasChange() const;

		void Undo(Scene& scene) override;
		void Redo(Scene& scene) override;
		const char* Name() const override { return "Transform"; }
		std::string Label() const override { return m_Label; }

	private:
		void Restore(Scene& scene, bool redo);
		std::vector<Entry> m_Entries;
		std::string m_Label;
	};
}
