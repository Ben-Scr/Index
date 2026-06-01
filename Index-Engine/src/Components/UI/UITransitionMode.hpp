#pragma once

#include <cstdint>

namespace Index {

	// ColorSwap: writes per-state Color into ImageComponent.Color. SpriteSwap: writes per-state UUID into TextureAssetId; unset slots fall back to NormalSprite. None: no automatic transition.
	enum class UITransitionMode : std::uint8_t {
		ColorSwap  = 0,
		SpriteSwap = 1,
		None       = 2,
	};

}
