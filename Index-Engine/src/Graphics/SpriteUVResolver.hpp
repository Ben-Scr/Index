#pragma once

#include "Core/Export.hpp"
#include "Core/UUID.hpp"

#include <cstdint>
#include <string_view>

namespace Index {

	struct INDEX_API SpriteUVRect {
		float U0 = 0.0f;
		float V0 = 0.0f;
		float U1 = 1.0f;
		float V1 = 1.0f;
		bool  IsFullTexture = true;
	};

	INDEX_API SpriteUVRect ResolveSpriteUVRect(UUID textureAssetId,
		std::string_view spriteName,
		int texPxW,
		int texPxH);

	INDEX_API void NotifySpriteSliceEpochBumped();

} // namespace Index
