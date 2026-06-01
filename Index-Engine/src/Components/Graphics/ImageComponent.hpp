#pragma once
#include "Core/UUID.hpp"
#include "Graphics/Filter.hpp"
#include "Graphics/TextureHandle.hpp"
#include "Collections/Color.hpp"

#include <cstdint>
#include <string>

namespace Index {
	struct ImageComponent {
		TextureHandle TextureHandle;
		UUID TextureAssetId{ 0 };
		Color Color;
		int16_t SortingOrder{ 0 };
		uint8_t SortingLayer{ 0 };
		// LEGACY: `.meta` import block wins; kept for back-compat.
		Filter FilterMode{ Filter::Bilinear };

		// Slice name in the bound texture's `.meta`. Empty = full texture.
		// Same semantics as SpriteRendererComponent::SpriteName.
		std::string SpriteName;
	};
}
