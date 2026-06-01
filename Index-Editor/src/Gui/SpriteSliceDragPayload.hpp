#pragma once

#include <cstring>
#include <string>
#include <string_view>

namespace Index {

	struct SpriteSliceDragPayload {
		char TexturePath[512];
		char SliceName[128];

		void Reset() {
			std::memset(TexturePath, 0, sizeof(TexturePath));
			std::memset(SliceName, 0, sizeof(SliceName));
		}

		void SetTexturePath(std::string_view path) {
			const std::size_t n = std::min(path.size(), sizeof(TexturePath) - 1);
			std::memcpy(TexturePath, path.data(), n);
			TexturePath[n] = '\0';
		}

		void SetSliceName(std::string_view name) {
			const std::size_t n = std::min(name.size(), sizeof(SliceName) - 1);
			std::memcpy(SliceName, name.data(), n);
			SliceName[n] = '\0';
		}
	};

	// Slice-aware targets check for this type first; non-slice targets ignore it and keep their existing ASSET_BROWSER_ITEM semantics unchanged.
	inline constexpr const char* k_SpriteSliceDragPayloadType = "ASSET_SPRITE_SLICE";

}
