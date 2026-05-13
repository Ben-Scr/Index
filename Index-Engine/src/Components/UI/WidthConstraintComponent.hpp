#pragma once

namespace Index {

	// Width Constraint — clamps the entity's RectTransform2D SizeDelta.x
	// to [MinWidth, MaxWidth] each frame. Mirrors Unity's LayoutElement
	// "ignore" convention: a negative bound disables that side of the
	// clamp, so MinWidth = -1 + MaxWidth = 200 caps width at 200 with no
	// lower bound, and both negative makes the component a no-op.
	//
	// Applied in UILayoutSystem AFTER ContentSizeFitter and text-wrap
	// natural sizing, so a CSF-fitted row whose children would push it
	// past MaxWidth gets clipped before any parent layout-group reads
	// the SizeDelta. Authored Width edits inside the range stay put;
	// values outside the range snap on the next layout pass.
	struct WidthConstraintComponent {
		float MinWidth = -1.0f;
		float MaxWidth = -1.0f;
	};

}
