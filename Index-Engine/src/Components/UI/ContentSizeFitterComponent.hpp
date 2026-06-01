#pragma once

namespace Index {

	// MUST run in UILayoutSystem before any layout-group on the parent reads this rect's size; nested fitters resolve outer-after-inner.
	struct ContentSizeFitterComponent {
		bool HorizontalFit = false;
		bool VerticalFit = false;

		float PaddingLeft = 0.0f;
		float PaddingRight = 0.0f;
		float PaddingTop = 0.0f;
		float PaddingBottom = 0.0f;
	};

}
