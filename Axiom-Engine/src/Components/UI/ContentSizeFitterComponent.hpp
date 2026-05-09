#pragma once

namespace Axiom {

	// Content Size Fitter — auto-resizes the entity's RectTransform2D
	// SizeDelta along enabled axes to fit the bounding box of its direct
	// children. Mirrors Unity's ContentSizeFitter (Preferred/Min/Unconstrained
	// → here we collapse to two booleans because there's no "preferred size"
	// concept on Axiom's child rects yet — the natural size is just the AABB).
	//
	// Runs in UILayoutSystem BEFORE any layout-group on the parent reads this
	// rect's size, so a HorizontalLayoutGroup containing a ContentSizeFitter-ed
	// row reflects the resized row. Recursive: nested fitters resolve outer
	// after inner because the outer fitter sees the inner's already-fitted
	// SizeDelta as its child's natural size.
	//
	// HorizontalFit / VerticalFit gate per-axis. Both off = component is a
	// no-op (matches Unity's Unconstrained default). Both on = fits both axes
	// to children. The dominant use case is one axis (e.g. a vertical menu
	// that grows downward to fit all entries) so the per-axis split matters.
	//
	// Padding (Left/Right/Top/Bottom) expands the fitted size beyond the
	// child bounding box on each side — children are not repositioned, so
	// the extra pixels read as empty margin around the content. Only the
	// axes whose Fit toggle is on absorb the corresponding padding pair.
	struct ContentSizeFitterComponent {
		bool HorizontalFit = false;
		bool VerticalFit = false;

		float PaddingLeft = 0.0f;
		float PaddingRight = 0.0f;
		float PaddingTop = 0.0f;
		float PaddingBottom = 0.0f;
	};

}
