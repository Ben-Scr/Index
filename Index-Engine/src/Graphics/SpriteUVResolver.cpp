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
		};

		std::unordered_map<uint64_t, CachedEntry> g_Cache;

		std::unordered_set<std::string> g_WarnedRefs;

		const std::vector<SpriteSlice>& GetCachedSlices(UUID textureAssetId) {
			const uint64_t epoch = g_SliceEpoch.load(std::memory_order_acquire);
			auto it = g_Cache.find(static_cast<uint64_t>(textureAssetId));
			if (it != g_Cache.end() && it->second.BuiltAtEpoch == epoch) {
				return it->second.Slices;
			}

			const std::string path = AssetRegistry::ResolvePath(static_cast<uint64_t>(textureAssetId));
			CachedEntry& entry = g_Cache[static_cast<uint64_t>(textureAssetId)];
			entry.BuiltAtEpoch = epoch;
			entry.Slices.clear();
			if (!path.empty()) {
				entry.Slices = AssetRegistry::ReadTextureMeta(path).Sprites;
			}
			return entry.Slices;
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
		// Hot path: empty name skips the cache lookup entirely. Most
		// SpriteRenderers in any existing scene hit this branch.
		if (spriteName.empty()) {
			return SpriteUVRect{};
		}
		if (static_cast<uint64_t>(textureAssetId) == 0 || texPxW <= 0 || texPxH <= 0) {
			return SpriteUVRect{};
		}

		const std::vector<SpriteSlice>& slices = GetCachedSlices(textureAssetId);
		const SpriteSlice* match = nullptr;
		for (const SpriteSlice& s : slices) {
			if (s.Name == spriteName) {
				match = &s;
				break;
			}
		}
		if (!match) {
			WarnOnceMissing(textureAssetId, spriteName);
			return SpriteUVRect{};
		}

		// Half-texel inset prevents adjacent-slice bleed under Bilinear filtering.
		const float fW = static_cast<float>(texPxW);
		const float fH = static_cast<float>(texPxH);
		SpriteUVRect uv;
		uv.U0 = (static_cast<float>(match->X) + 0.5f) / fW;
		uv.V0 = (static_cast<float>(match->Y) + 0.5f) / fH;
		uv.U1 = (static_cast<float>(match->X + match->W) - 0.5f) / fW;
		uv.V1 = (static_cast<float>(match->Y + match->H) - 0.5f) / fH;
		uv.IsFullTexture = false;
		return uv;
	}

	void NotifySpriteSliceEpochBumped() {
		g_SliceEpoch.fetch_add(1, std::memory_order_acq_rel);
		g_WarnedRefs.clear();
	}

} // namespace Index
