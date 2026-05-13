#pragma once

namespace Index {

	// Mask — apply to a UI entity so its descendants' rendering is
	// clipped to the entity's resolved rect. ScrollView / ScrollRect
	// puts a Mask on its Viewport so content scrolled past the
	// viewport edges is hidden instead of bleeding into surrounding
	// UI.
	//
	// ShowMaskGraphic
	//   When true, the mask entity's own ImageComponent (if any) still
	//   renders normally — useful when the mask doubles as the visible
	//   background of the masked region. When false, the entity's image
	//   is suppressed at draw time and only the clipping effect remains.
	//
	// Nesting: a descendant under multiple Mask ancestors is clipped
	// to the intersection of all their rects, so a Mask inside a Mask
	// narrows the visible area further.
	//
	// Implementation note: GuiRenderer applies the clip using a
	// glScissor rect derived from the mask entity's resolved AABB.
	// This is axis-aligned only — rotated masks fall back to the
	// AABB of the rotated rect, which is a superset of the rotated
	// shape (some content just outside the rotated rect may still
	// render). Stencil-based exact clipping is a future upgrade.
	struct MaskComponent {
		bool ShowMaskGraphic = true;
	};

}
