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
		// Sampler filter applied to the bound texture. LEGACY: persists in
		// the scene file for back-compat (older scenes embedded a per-entity
		// filter) but the asset inspector is now the only authoring surface.
		// The Sprite Editor's `.meta` import block wins over this field at
		// texture load time.
		Filter FilterMode{ Filter::Bilinear };

		// Which slice of the bound texture to draw. Empty = the full texture
		// (current behaviour, what every existing scene has). Non-empty =
		// the name of a SpriteSlice authored in the Sprite Editor and
		// persisted in the texture's `.meta`. The renderer resolves this
		// to a UV rect every frame via SpriteUVResolver; lookups are O(1)
		// against a cached per-texture slice map keyed on a slice-epoch
		// counter that the Sprite Editor's Apply path bumps.
		std::string SpriteName;
	};
}
