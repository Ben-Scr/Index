#include <pch.hpp>
#include "Undo/TransformEditCommand.hpp"

#include "Scene/Scene.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Systems/TransformHierarchySystem.hpp"

namespace Index {

	namespace {
		bool SnapshotsDiffer(const TransformSnapshot& a, const TransformSnapshot& b) {
			const bool same = a.Position == b.Position && a.Scale == b.Scale && a.Rotation == b.Rotation
				&& a.LocalPosition == b.LocalPosition && a.LocalScale == b.LocalScale
				&& a.LocalRotation == b.LocalRotation;
			return !same;
		}

		void WriteSnapshot(Transform2DComponent& transform, const TransformSnapshot& s) {
			transform.Position = s.Position;
			transform.Scale = s.Scale;
			transform.Rotation = s.Rotation;
			transform.LocalPosition = s.LocalPosition;
			transform.LocalScale = s.LocalScale;
			transform.LocalRotation = s.LocalRotation;
			transform.MarkDirty();
		}
	}

	bool TransformEditCommand::HasChange() const {
		for (const Entry& e : m_Entries) {
			if (SnapshotsDiffer(e.Before, e.After)) return true;
		}
		return false;
	}

	void TransformEditCommand::Undo(Scene& scene) { Restore(scene, false); }
	void TransformEditCommand::Redo(Scene& scene) { Restore(scene, true); }

	void TransformEditCommand::Restore(Scene& scene, bool redo) {
		for (const Entry& e : m_Entries) {
			EntityHandle handle = entt::null;
			// Resolve by persistent id, not a stored handle — entt recycles handles
			// and a reload reassigns them, but the UUID is stable.
			if (!scene.TryResolveEntityRef(e.PersistentId, handle)) continue;
			Transform2DComponent* transform = nullptr;
			if (!scene.TryGetComponent<Transform2DComponent>(handle, transform) || !transform) continue;
			WriteSnapshot(*transform, redo ? e.After : e.Before);
		}
		// Re-derive world transforms from the restored locals in one pass.
		TransformHierarchySystem::Propagate(scene);
		scene.MarkDirty();
	}
}
