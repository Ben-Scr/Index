#pragma once

#include "Collections/Vec2.hpp"
#include "Components/UI/UIAlignment.hpp"

namespace Index {

	// Lays out an entity's children left-to-right (or right-to-left) inside
	// the parent rect. Mirrors Unity's HorizontalLayoutGroup: every direct
	// child with a RectTransform2DComponent is repositioned each frame so
	// they sit in a single row separated by Spacing pixels, respecting the
	// parent's Padding insets.
	//
	// ChildAlignment controls vertical placement of each child within the
	// laid-out row (Upper / Middle / Lower) and horizontal placement of the
	// whole row inside the parent (Left / Center / Right).
	//
	// ControlChildSize lets the layout overwrite each child's SizeDelta so
	// it expands to fill the row's height (Y) and the equally-divided slot
	// width (X). When false the child keeps its authored SizeDelta — useful
	// when child sizes are content-driven and you only want spacing /
	// alignment.
	//
	// ChildForceExpand fills any leftover space after the natural sizes have
	// been laid out, distributing it equally across children. Pairs with
	// ControlChildSize: when both are on, every child gets an equal slot;
	// when only ControlChildSize is on, children take their authored width
	// and the row is left-aligned with leftover space at the end.
	struct HorizontalLayoutGroupComponent {
		// Padding inset (Left, Right, Top, Bottom) inside the parent rect
		// where children are laid out, in pixels.
		float PaddingLeft = 0.0f;
		float PaddingRight = 0.0f;
		float PaddingTop = 0.0f;
		float PaddingBottom = 0.0f;

		// Pixels of empty space between adjacent children.
		float Spacing = 0.0f;

		// 9-cell alignment grid; see UIAlignment.hpp.
		UIAlignment ChildAlignment = UIAlignment::MiddleLeft;

		// When true the layout reverses iteration order so children flow
		// right-to-left. The first child still owns the leftmost slot in
		// the registry's order; this just flips the visual ordering.
		bool ReverseArrangement = false;

		// Overwrite child SizeDelta width / height to match the row's
		// computed slot. When off the layout only positions children.
		// Default is OFF — most users want children to keep their
		// authored sizes; opt in by ticking the box when you want the
		// layout to drive width/height.
		bool ControlChildWidth = false;
		bool ControlChildHeight = false;

		// When true the layout multiplies each child's contribution to the
		// row's natural size by the child's RectTransform2D.LocalScale, so a
		// scaled-up child reserves more space and a scaled-down child takes
		// less. When false the child's authored SizeDelta is used directly.
		// Mirrors Unity's "Use Child Scale" toggle. Per-axis split lets
		// width-only / height-only setups work without cross-axis weirdness.
		bool UseChildScaleWidth = false;
		bool UseChildScaleHeight = false;

		// After natural / controlled sizes are computed, distribute any
		// leftover horizontal / vertical space equally to children.
		bool ChildForceExpandWidth = true;
		bool ChildForceExpandHeight = true;
	};

}
