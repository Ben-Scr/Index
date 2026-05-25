#pragma once
#include "Core/Export.hpp"
#include "Collections/Vec2.hpp"
#include "Scene/EntityHandle.hpp"

namespace Index {
	class Scene;

	// AABB-based "what entity is at this world point" query. Shared by the
	// editor viewport's click-to-select handler and the C# scripting
	// EntityPicker.TryPickEntity binding so both surfaces stay in lockstep.
	//
	// Hit-test rules:
	//   - Transform2DComponent entities use AABB::FromTransform (Position ±
	//     Scale/2, rotated). Sprite renderers break ties by SortingLayer then
	//     SortingOrder (highest = on top).
	//   - RectTransform2DComponent entities (UI, mutually exclusive with
	//     Transform2D — see DeclareConflict in BuiltInComponentRegistration)
	//     are world-projected via GuiRenderer::ComputeWorldUIPixelScale and
	//     always override world-space hits because UI draws on top. Among UI
	//     hits, later iterated wins (matches render order).
	//
	// Caller must ensure TransformHierarchySystem::Propagate / ComputeUILayout
	// have run for the frame — the editor calls them as part of
	// RenderSceneIntoFBO, scripts run after the per-frame system update.
	class INDEX_API EntityPicker {
	public:
		static bool TryPickEntity(Scene& scene, const Vec2& worldPoint, EntityHandle& outEntity);
	};
}
