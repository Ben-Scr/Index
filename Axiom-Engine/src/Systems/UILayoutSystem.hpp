#pragma once
#include "Core/Export.hpp"
#include "Scene/ISystem.hpp"

namespace Axiom {

	// Walk every RectTransform2DComponent in the scene and write the
	// resolved screen-space AABB (centered window-pixel coords) into
	// ResolvedMin/ResolvedMax. Hierarchy-aware: a child's anchors and
	// pivot apply against its parent's resolved rect (or the full
	// window viewport when the entity is a root).
	//
	// Exposed as a free function so it can run from BOTH UILayoutSystem
	// (in play mode, before UIEventSystem) and UIRenderer (every frame,
	// including editor mode, after UIEventSystem mutates authored
	// values like the slider handle's AnchoredPosition). Marked
	// AXIOM_API so the editor can refresh layout before reading
	// resolved coords for gizmo overlays.
	AXIOM_API void ComputeUILayout(Scene& scene);

	// Walks every RectTransform2DComponent in the scene each frame and
	// writes the resolved screen-space AABB into ResolvedMin / ResolvedMax.
	// The walk is hierarchy-aware: a child's anchors and pivot are applied
	// against its parent's resolved rect (or the full window viewport
	// when the entity is a root).
	//
	// MUST run before UIEventSystem (so hit-tests use up-to-date rects).
	// UIRenderer re-runs ComputeUILayout itself before drawing so the
	// editor view (which doesn't tick scene systems) still shows the
	// correct stretched layout.
	class UILayoutSystem : public ISystem {
	public:
		void Update(Scene& scene) override;
	};

}
