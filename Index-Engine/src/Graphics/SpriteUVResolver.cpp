#include "pch.hpp"
#include "Graphics/SpriteUVResolver.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Assets/TextureMeta.hpp"
#include "Core/Log.hpp"

#include <atomic>
#include <mutex>
#include <shared_mutex>
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
		// g_Cache is read/built from the parallel sprite-collect path (multiple worker
		// threads, see Renderer2D ParallelFor). shared_mutex: shared lock on a cache
		// hit, unique lock only on the cold build path. A reference into the map must
		// never escape the lock (a concurrent rehash invalidates it), so callers
		// extract the small UV result while holding the lock.
		std::shared_mutex g_CacheMutex;

		std::unordered_set<std::string> g_WarnedRefs;
		std::mutex g_WarnMutex; // guards g_WarnedRefs; always taken after g_CacheMutex

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
			std::lock_guard<std::mutex> warnLock(g_WarnMutex);
			if (g_WarnedRefs.insert(std::move(key)).second) {
				IDX_CORE_WARN_TAG("SpriteUVResolver",
					"Sprite '{}' not found in texture meta for asset {}. Falling back to full texture.",
					spriteName, static_cast<uint64_t>(textureAssetId));
			}
		}

		// Reads an already-built entry and returns just the small UV result, so no
		// reference into g_Cache escapes the caller's lock. Caller must hold g_CacheMutex
		// (shared or unique).
		SpriteUVRect ExtractUVLocked(const CachedEntry& entry, UUID textureAssetId,
			std::string_view spriteName, int texPxW, int texPxH)
		{
			// Empty name = "single sprite": fall back to the per-texture crop if
			// one is authored, else the full texture.
			if (spriteName.empty()) {
				if (entry.Crop.Enabled && entry.Crop.W > 0 && entry.Crop.H > 0) {
					return RectToUV(entry.Crop.X, entry.Crop.Y, entry.Crop.W, entry.Crop.H, texPxW, texPxH);
				}
				return SpriteUVRect{};
			}

			for (const SpriteSlice& s : entry.Slices) {
				if (s.Name == spriteName) {
					return RectToUV(s.X, s.Y, s.W, s.H, texPxW, texPxH);
				}
			}
			WarnOnceMissing(textureAssetId, spriteName);
			return SpriteUVRect{};
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

		const uint64_t epoch = g_SliceEpoch.load(std::memory_order_acquire);
		const uint64_t key   = static_cast<uint64_t>(textureAssetId);

		// Fast path: cache hit under a shared lock — concurrent worker reads are safe.
		{
			std::shared_lock<std::shared_mutex> readLock(g_CacheMutex);
			auto it = g_Cache.find(key);
			if (it != g_Cache.end() && it->second.BuiltAtEpoch == epoch) {
				return ExtractUVLocked(it->second, textureAssetId, spriteName, texPxW, texPxH);
			}
		}

		// Slow path: build (or rebuild a stale entry) under a unique lock. Re-check
		// first in case another thread built it between dropping the read lock and
		// taking the write lock.
		std::unique_lock<std::shared_mutex> writeLock(g_CacheMutex);
		CachedEntry& entry = g_Cache[key];
		if (entry.BuiltAtEpoch != epoch) {
			const std::string path = AssetRegistry::ResolvePath(key);
			entry.BuiltAtEpoch = epoch;
			entry.Slices.clear();
			entry.Crop = TextureCrop{};
			if (!path.empty()) {
				TextureMeta meta = AssetRegistry::ReadTextureMeta(path);
				entry.Slices = std::move(meta.Sprites);
				entry.Crop   = meta.Crop;
			}
		}
		return ExtractUVLocked(entry, textureAssetId, spriteName, texPxW, texPxH);
	}

	void NotifySpriteSliceEpochBumped() {
		g_SliceEpoch.fetch_add(1, std::memory_order_acq_rel);
		std::lock_guard<std::mutex> warnLock(g_WarnMutex);
		g_WarnedRefs.clear();
	}

} // namespace Index
