#pragma once

#include "Collections/Vec2.hpp"
#include "Components/UI/UIAlignment.hpp"

namespace Axiom {

	// Lays out an entity's children top-to-bottom (or bottom-to-top) inside
	// the parent rect. Vertical sibling of HorizontalLayoutGroupComponent —
	// see that file for the field semantics. This component flips the
	// dominant axis: children sit in a single column instead of a row, and
	// ChildForceExpandHeight grows leftover vertical space rather than
	// horizontal.
	struct VerticalLayoutGroupComponent {
		float PaddingLeft = 0.0f;
		float PaddingRight = 0.0f;
		float PaddingTop = 0.0f;
		float PaddingBottom = 0.0f;

		float Spacing = 0.0f;

		// 9-cell alignment grid; see UIAlignment.hpp.
		UIAlignment ChildAlignment = UIAlignment::UpperLeft;

		// When true children flow bottom-to-top instead of top-to-bottom.
		bool ReverseArrangement = false;

		// Default OFF — see HorizontalLayoutGroupComponent for the reasoning.
		bool ControlChildWidth = false;
		bool ControlChildHeight = false;

		// See HorizontalLayoutGroupComponent — per-axis "Use Child Scale"
		// flags that fold child LocalScale into the natural-size computation.
		bool UseChildScaleWidth = false;
		bool UseChildScaleHeight = false;

		bool ChildForceExpandWidth = true;
		bool ChildForceExpandHeight = true;
	};

}
