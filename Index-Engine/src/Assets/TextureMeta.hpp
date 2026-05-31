#pragma once

#include "Core/Export.hpp"
#include "Graphics/Filter.hpp"
#include "Graphics/Wrap.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Index {

	// One sliced sprite within a texture asset's `.meta`. Rectangle coordinates
	// are in TEXTURE PIXELS with (0,0) at the top-left of the source image —
	// the natural authoring frame in the Sprite Editor and the convention
	// `stb_image` produces when re-decoding for the auto-slice algorithm.
	//
	// `Name` is the persisted reference key used by SpriteRendererComponent::SpriteName
	// (rename-stable across slice reorders), so it MUST be unique within a
	// single texture's slice list — the Sprite Editor's commit path enforces
	// that.
	struct INDEX_API SpriteSlice {
		std::string Name;
		int X = 0;
		int Y = 0;
		int W = 0;
		int H = 0;
		// Pivot in 0..1 slice-local space. (0.5, 0.5) = centre; (0, 0) = top-
		// left; (0.5, 0) = top-centre; etc. Used by future anchored placement
		// (sprite hangers, particle emitter anchors). Stored even when unused
		// so the schema doesn't need a follow-up bump to add it later.
		float PivotX = 0.5f;
		float PivotY = 0.5f;
	};

	// Author-time sampler settings persisted in the texture's companion `.meta`
	// file. TextureManager::LoadTextureByUUID consults these on load and they
	// win over caller-supplied defaults — that's how Filter / Wrap dropdowns in
	// the asset inspector actually stick across editor sessions and into
	// standalone builds.
	struct INDEX_API TextureImportSettings {
		Filter FilterMode = Filter::Bilinear;
		Wrap   WrapU      = Wrap::Clamp;
		Wrap   WrapV      = Wrap::Clamp;
	};

	// In-memory view of a texture's `.meta` import block + slice list.
	//
	// `HasImportBlock` distinguishes "the meta file omitted import settings"
	// from "the user explicitly chose Bilinear/Clamp/Clamp". The loader needs
	// that distinction so a freshly-imported texture (no meta yet) keeps the
	// caller's default sampler rather than silently being overridden by the
	// struct defaults above.
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
