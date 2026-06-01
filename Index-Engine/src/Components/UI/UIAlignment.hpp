#pragma once
#include <cstdint>

namespace Index {

	// 9-cell grid: row = value / 3, column = value % 3, top-to-bottom / left-to-right.
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
