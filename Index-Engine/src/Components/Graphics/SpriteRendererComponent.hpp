#pragma once
#include "Core/Export.hpp"
#include "Core/UUID.hpp"
#include "Graphics/Filter.hpp"
#include "Graphics/TextureHandle.hpp"
#include "Collections/Color.hpp"

#include <string>

namespace Index {
	struct INDEX_API SpriteRendererComponent {
		SpriteRendererComponent() = default;

		short SortingOrder{0};
		uint8_t SortingLayer{0};
		TextureHandle TextureHandle;
		UUID TextureAssetId{ 0 };
		Color Color{ 1.0f, 1.0f, 1.0f, 1.0f };
		// LEGACY: `.meta` import block wins over this field at texture load time; kept for back-compat.
		Filter FilterMode{ Filter::Bilinear };

		std::string SpriteName;
	};
}
