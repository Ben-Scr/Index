#pragma once

#include <cstring>
#include <string>
#include <string_view>

namespace Index {

	// Drag-drop payload for sprite-sheet slices. Carries enough information for
	// the receiver to resolve both the parent texture (via path → AssetRegistry
	// UUID) and the picked sub-rect (slice name → SpriteUVResolver).
	//
	// Sources (anywhere a user can grab a slice):
	//   - AssetBrowser: per-slice tile rendered when the user expands a sliced
	//     sprite sheet.
	//   - ReferencePicker: future drag-out of slice entries (currently picker
	//     uses click-to-select only).
	//
	// Sinks (anywhere a texture reference can land):
	//   - PropertyDrawer's `DrawAssetRefByKind` (Texture kind) — accepts the
	//     payload, sets the texture UUID, and *also* forwards the slice name
	//     via PropertyValue::StringValue so slice-aware setters (SpriteRenderer,
	//     Image) can apply it atomically. Non-slice-aware texture refs
	//     silently drop the slice name and keep working unchanged.
	//
	// The arrays are sized to the largest realistic values we ship — the
	// project root is typically deep, and slice names are user-typed
	// identifiers. Both are null-terminated; the receiver should treat them as
	// C strings and reject any payload whose `TexturePath` is empty.
	struct SpriteSliceDragPayload {
		char TexturePath[512];
		char SliceName[128];

		void Reset() {
			std::memset(TexturePath, 0, sizeof(TexturePath));
			std::memset(SliceName, 0, sizeof(SliceName));
		}

		// Safe copy helpers — silently truncate when the source is longer than
		// the buffer. Both paths are produced by the asset registry (already
		// canonical) and slice names cap at the Sprite Editor's UI limit, so
		// truncation is a defence-in-depth measure rather than an expected case.
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

	// ImGui payload type string. Distinct from "ASSET_BROWSER_ITEM" so existing
	// drop targets keep their semantics unchanged — they accept full textures,
	// not slices — and slice-aware targets opt in explicitly by checking for
	// this type first.
	inline constexpr const char* k_SpriteSliceDragPayloadType = "ASSET_SPRITE_SLICE";

}
