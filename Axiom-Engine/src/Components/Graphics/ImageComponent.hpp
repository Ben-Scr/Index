#pragma once
#include "Core/UUID.hpp"
#include "Graphics/TextureHandle.hpp"
#include "Collections/Color.hpp"

#include <cstdint>

namespace Axiom {
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
	};
}
