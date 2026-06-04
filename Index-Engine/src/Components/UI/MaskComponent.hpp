#pragma once

namespace Index {

	// Clips descendants to this entity's rect and, when an Image is present,
	// to the Image texture's alpha so transparent/clear pixels do not pass.
	struct MaskComponent {
		bool ShowMaskGraphic = true;
	};

}
