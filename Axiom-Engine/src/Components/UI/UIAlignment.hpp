#pragma once
#include <cstdint>

namespace Axiom {

	// 9-cell alignment grid shared by HorizontalLayoutGroup,
	// VerticalLayoutGroup, GridLayoutGroup and any future widget that
	// needs to align children inside a parent rect. Encoded as a small
	// integer so the layout-pass math (which historically did `% 3`
	// for column / `/ 3` for row) keeps working without translation —
	// the enum is purely a typing/inspector improvement.
	//
	// Layout convention (matches Unity): row index = value / 3,
	// column index = value % 3, with rows ordered top → bottom and
	// columns left → right.
	enum class UIAlignment : std::uint8_t {
		UpperLeft   = 0,
		UpperCenter = 1,
		UpperRight  = 2,
		MiddleLeft  = 3,
		MiddleCenter= 4,
		MiddleRight = 5,
		LowerLeft   = 6,
		LowerCenter = 7,
		LowerRight  = 8,
	};

}
