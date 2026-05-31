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
		// Same shape as SpriteRendererComponent so the inspector / serializer /
		// script API can mirror that component verbatim. GuiRenderer sorts
		// images first by SortingLayer, then by SortingOrder, with hierarchy
		// walk order as the final tiebreaker (so siblings keep author order
		// when explicit sort fields tie).
		int16_t SortingOrder{ 0 };
		uint8_t SortingLayer{ 0 };
		// Legacy per-entity sampler filter. The Sprite Editor's `.meta`
		// import block is now the authoritative source; this field still
		// loads/saves for back-compat but is no longer the authoring
		// surface.
		Filter FilterMode{ Filter::Bilinear };

		// Slice name in the bound texture's `.meta`. Empty = full texture.
		// Same semantics as SpriteRendererComponent::SpriteName.
		std::string SpriteName;
	};
}
