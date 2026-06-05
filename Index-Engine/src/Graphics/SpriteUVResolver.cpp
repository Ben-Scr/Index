#include "pch.hpp"
#include "Graphics/SpriteUVResolver.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Assets/TextureMeta.hpp"
#include "Core/Log.hpp"

#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Index {

	namespace {
		std::atomic<uint64_t> g_SliceEpoch{ 1 };

		struct CachedEntry {
			uint64_t                 BuiltAtEpoch = 0;
			std::vector<SpriteSlice> Slices;
			TextureCrop              Crop;
		};

		std::unordered_map<uint64_t, CachedEntry> g_Cache;

		std::unordered_set<std::string> g_WarnedRefs;

		const CachedEntry& GetCachedEntry(UUID textureAssetId) {
			const uint64_t epoch = g_SliceEpoch.load(std::memory_order_acquire);
			auto it = g_Cache.find(static_cast<uint64_t>(textureAssetId));
			if (it != g_Cache.end() && it->second.BuiltAtEpoch == epoch) {
				return it->second;
			}

			const std::string path = AssetRegistry::ResolvePath(static_cast<uint64_t>(textureAssetId));
			CachedEntry& entry = g_Cache[static_cast<uint64_t>(textureAssetId)];
			entry.BuiltAtEpoch = epoch;
			entry.Slices.clear();
			entry.Crop = TextureCrop{};
			if (!path.empty()) {
				TextureMeta meta = AssetRegistry::ReadTextureMeta(path);
				entry.Slices = std::move(meta.Sprites);
				entry.Crop   = meta.Crop;
			}
			return entry;
		}

		// Slice/crop rect (texture pixels) → UV, with a half-texel inset to
		// stop adjacent-slice bleed under Bilinear filtering.
		SpriteUVRect RectToUV(int x, int y, int w, int h, int texPxW, int texPxH) {
			const float fW = static_cast<float>(texPxW);
			const float fH = static_cast<float>(texPxH);
			SpriteUVRect uv;
			uv.U0 = (static_cast<float>(x)     + 0.5f) / fW;
			uv.V0 = (static_cast<float>(y)     + 0.5f) / fH;
			uv.U1 = (static_cast<float>(x + w) - 0.5f) / fW;
			uv.V1 = (static_cast<float>(y + h) - 0.5f) / fH;
			uv.IsFullTexture = false;
			return uv;
		}

		void WarnOnceMissing(UUID textureAssetId, std::string_view spriteName) {
			std::string key = std::to_string(static_cast<uint64_t>(textureAssetId));
			key.push_back('|');
			key.append(spriteName);
			if (g_WarnedRefs.insert(std::move(key)).second) {
				IDX_CORE_WARN_TAG("SpriteUVResolver",
					"Sprite '{}' not found in texture meta for asset {}. Falling back to full texture.",
					spriteName, static_cast<uint64_t>(textureAssetId));
			}
		}
	} // namespace

	SpriteUVRect ResolveSpriteUVRect(UUID textureAssetId,
		std::string_view spriteName,
		int texPxW,
		int texPxH)
	{
		// No asset id (e.g. procedural texture) — nothing to look up.
		if (static_cast<uint64_t>(textureAssetId) == 0 || texPxW <= 0 || texPxH <= 0) {
			return SpriteUVRect{};
		}

		const CachedEntry& entry = GetCachedEntry(textureAssetId);

		// Empty name = "single sprite": fall back to the per-texture crop if
		// one is authored, else the full texture.
		if (spriteName.empty()) {
			if (entry.Crop.Enabled && entry.Crop.W > 0 && entry.Crop.H > 0) {
				return RectToUV(entry.Crop.X, entry.Crop.Y, entry.Crop.W, entry.Crop.H, texPxW, texPxH);
			}
			return SpriteUVRect{};
		}

		const SpriteSlice* match = nullptr;
		for (const SpriteSlice& s : entry.Slices) {
			if (s.Name == spriteName) {
				match = &s;
				break;
			}
		}
		if (!match) {
			WarnOnceMissing(textureAssetId, spriteName);
			return SpriteUVRect{};
		}

		return RectToUV(match->X, match->Y, match->W, match->H, texPxW, texPxH);
	}

	void NotifySpriteSliceEpochBumped() {
		g_SliceEpoch.fetch_add(1, std::memory_order_acq_rel);
		g_WarnedRefs.clear();
	}

} // namespace Index
