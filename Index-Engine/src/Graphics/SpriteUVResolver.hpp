#pragma once

#include "Core/Export.hpp"
#include "Core/UUID.hpp"

#include <cstdint>
#include <string_view>

namespace Index {

	// Normalised UV rect (0..1, sampler-space) the renderer hands to the
	// sprite shader. The shader remaps the unit-quad UV through this rect so
	// the sprite samples only the slice's pixels. `IsFullTexture` lets the
	// renderer avoid even reading the rest of the struct when no slicing
	// applies — covers every legacy SpriteRenderer that hasn't been pointed
	// at a slice.
	struct INDEX_API SpriteUVRect {
		float U0 = 0.0f;
		float V0 = 0.0f;
		float U1 = 1.0f;
		float V1 = 1.0f;
		bool  IsFullTexture = true;
	};

	// Resolve a (texture asset, sprite name) pair into the UV rect that
	// should be packed into the per-instance vertex stream.
	//
	// • Empty `spriteName` returns the full-texture rect immediately —
	//   that's the path every legacy SpriteRenderer hits.
	// • Unknown `spriteName` (no matching SpriteSlice in the texture's
	//   `.meta`) also returns the full-texture rect, but emits a one-shot
	//   warning per (textureAssetId, spriteName) pair so the editor log
	//   surfaces the stale reference without spamming a frame.
	// • Known `spriteName` returns the slice's pixel rect converted to
	//   normalised sampler space with a half-texel inset (avoids bleed
	//   into adjacent slices under Bilinear filtering of tight atlases).
	//
	// The lookup hits a per-texture slice map cached on a "slice epoch"
	// counter. The Sprite Editor's Apply path bumps the epoch via
	// NotifySpriteSliceEpochBumped() which invalidates every cached entry
	// in O(1) (lazy refill on next ResolveSpriteUVRect call).
	INDEX_API SpriteUVRect ResolveSpriteUVRect(UUID textureAssetId,
		std::string_view spriteName,
		int texPxW,
		int texPxH);

	// Called by SpriteEditorPanel::Apply (Phase C). Bumps the slice epoch
	// so the next ResolveSpriteUVRect call for any cached texture refills
	// from the freshly-written `.meta`. Safe to call from any thread that
	// also drives the main render loop — the resolver is main-thread only.
	INDEX_API void NotifySpriteSliceEpochBumped();

} // namespace Index
