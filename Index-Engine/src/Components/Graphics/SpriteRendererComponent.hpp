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
		// Default = use the texture's own (.meta/import) filter; a concrete value
		// overrides it. The texture's .meta still wins at load time regardless.
		Filter FilterMode{ Filter::Default };

		std::string SpriteName;
	};
}
