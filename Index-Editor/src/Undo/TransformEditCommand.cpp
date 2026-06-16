#include <pch.hpp>
#include "Undo/TransformEditCommand.hpp"

#include "Scene/Scene.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Systems/TransformHierarchySystem.hpp"

namespace Index {

	EntityTransformSnapshot CaptureEntityTransform(Scene& scene, EntityHandle handle) {
		EntityTransformSnapshot snap;
		// RectTransform2D and Transform2D are mutually exclusive; UI rects first.
		RectTransform2DComponent* rect = nullptr;
		if (scene.TryGetComponent<RectTransform2DComponent>(handle, rect) && rect) {
			snap.Valid = true;
			snap.IsRect = true;
			snap.Rect = RectTransformSnapshot{
				rect->AnchorMin, rect->AnchorMax, rect->Pivot,
				rect->AnchoredPosition, rect->SizeDelta,
				rect->LocalRotation, rect->LocalScale };
			return snap;
		}
		Transform2DComponent* tf = nullptr;
		if (scene.TryGetComponent<Transform2DComponent>(handle, tf) && tf) {
			snap.Valid = true;
			snap.IsRect = false;
			snap.Transform = TransformSnapshot{
				tf->Position, tf->Scale, tf->Rotation,
				tf->LocalPosition, tf->LocalScale, tf->LocalRotation };
		}
		return snap;
	}

	void WriteEntityTransform(Scene& scene, EntityHandle handle, const EntityTransformSnapshot& snap) {
		if (!snap.Valid) return;
		if (snap.IsRect) {
			RectTransform2DComponent* rect = nullptr;
			if (!scene.TryGetComponent<RectTransform2DComponent>(handle, rect) || !rect) return;
			rect->AnchorMin = snap.Rect.AnchorMin;
			rect->AnchorMax = snap.Rect.AnchorMax;
			rect->Pivot = snap.Rect.Pivot;
			rect->AnchoredPosition = snap.Rect.AnchoredPosition;
			rect->SizeDelta = snap.Rect.SizeDelta;
			rect->LocalRotation = snap.Rect.LocalRotation;
			rect->LocalScale = snap.Rect.LocalScale;
			// UILayoutSystem re-resolves the world rect every frame; no dirty flag.
			return;
		}
		Transform2DComponent* tf = nullptr;
		if (!scene.TryGetComponent<Transform2DComponent>(handle, tf) || !tf) return;
		// Restore both world and local — the gizmo writes both, and the world
		// fields are what the renderer reads directly (the hierarchy pass that
		// would re-derive them is gated, so don't rely on it for the entity itself).
		tf->Position = snap.Transform.Position;
		tf->Scale = snap.Transform.Scale;
		tf->Rotation = snap.Transform.Rotation;
		tf->LocalPosition = snap.Transform.LocalPosition;
		tf->LocalScale = snap.Transform.LocalScale;
		tf->LocalRotation = snap.Transform.LocalRotation;
		tf->MarkDirty();
		// Flag at the scene level too so children follow on the next pass.
		scene.MarkTransformDirty(handle);
	}

	namespace {
		bool TransformSnapshotsDiffer(const TransformSnapshot& a, const TransformSnapshot& b) {
			return !(a.Position == b.Position && a.Scale == b.Scale && a.Rotation == b.Rotation
				&& a.LocalPosition == b.LocalPosition && a.LocalScale == b.LocalScale
				&& a.LocalRotation == b.LocalRotation);
		}
		bool RectSnapshotsDiffer(const RectTransformSnapshot& a, const RectTransformSnapshot& b) {
			return !(a.AnchorMin == b.AnchorMin && a.AnchorMax == b.AnchorMax && a.Pivot == b.Pivot
				&& a.AnchoredPosition == b.AnchoredPosition && a.SizeDelta == b.SizeDelta
				&& a.LocalRotation == b.LocalRotation && a.LocalScale == b.LocalScale);
		}
	}

	bool EntityTransformsDiffer(const EntityTransformSnapshot& a, const EntityTransformSnapshot& b) {
		if (a.Valid != b.Valid || a.IsRect != b.IsRect) return true;
		if (!a.Valid) return false;
		return a.IsRect ? RectSnapshotsDiffer(a.Rect, b.Rect)
			: TransformSnapshotsDiffer(a.Transform, b.Transform);
	}

	bool TransformEditCommand::HasChange() const {
		for (const Entry& e : m_Entries) {
			if (EntityTransformsDiffer(e.Before, e.After)) return true;
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
			WriteEntityTransform(scene, handle, redo ? e.After : e.Before);
		}
		// Re-derive world transforms from the restored locals in one pass.
		TransformHierarchySystem::Propagate(scene);
		scene.MarkDirty();
	}
}
