#pragma once

#include "Collections/Vec2.hpp"
#include "Components/UI/UIAlignment.hpp"

namespace Index {

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

		bool ReverseArrangement = false;

		bool ControlChildWidth = false;
		bool ControlChildHeight = false;

		bool UseChildScaleWidth = false;
		bool UseChildScaleHeight = false;

		bool ChildForceExpandWidth = true;
		bool ChildForceExpandHeight = true;
	};

}
