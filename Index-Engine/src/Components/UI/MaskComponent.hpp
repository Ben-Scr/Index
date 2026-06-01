#pragma once

namespace Index {

	// Clips descendants to this entity's rect. Axis-aligned scissor only — rotated masks use the AABB, so some content outside the rotated shape may still render.
	struct MaskComponent {
		bool ShowMaskGraphic = true;
	};

}
