#pragma once

#include "Core/Export.hpp"
#include "Graphics/Filter.hpp"
#include "Graphics/Wrap.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Index {

	struct INDEX_API SpriteSlice {
		std::string Name;
		int X = 0;
		int Y = 0;
		int W = 0;
		int H = 0;
		float PivotX = 0.5f;
		float PivotY = 0.5f;
	};

	struct INDEX_API TextureImportSettings {
		Filter FilterMode = Filter::Bilinear;
		Wrap   WrapU      = Wrap::Clamp;
		Wrap   WrapV      = Wrap::Clamp;
	};

	// HasImportBlock distinguishes absent meta (keep caller default) from explicit Bilinear/Clamp/Clamp authored by the user.
	struct INDEX_API TextureMeta {
		TextureImportSettings Import;
		std::vector<SpriteSlice> Sprites;
		bool HasImportBlock = false;

		const SpriteSlice* FindByName(std::string_view name) const {
			for (const SpriteSlice& slice : Sprites) {
				if (slice.Name == name) {
					return &slice;
				}
			}
			return nullptr;
		}

		int IndexOfName(std::string_view name) const {
			for (std::size_t i = 0; i < Sprites.size(); ++i) {
				if (Sprites[i].Name == name) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}
	};

} // namespace Index
