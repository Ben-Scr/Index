#pragma once

namespace Index {

	// Clamps SizeDelta.x to [MinWidth, MaxWidth]; negative = disabled. Applied after ContentSizeFitter so layout groups read the clamped value.
	struct WidthConstraintComponent {
		float MinWidth = -1.0f;
		float MaxWidth = -1.0f;
	};

}
