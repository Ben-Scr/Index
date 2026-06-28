#include "pch.hpp"
#include "Graphics/Text/FontManager.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Core/Application.hpp"
#include "Core/Log.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Path.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <utility>


namespace Index {

	namespace {
		// Engine frame counter; 0 when no Application (headless/tests — eviction
		// never runs there anyway). Used to protect the current frame's working set.
		int CurrentFrame() {
			const Application* app = Application::GetInstance();
			return app ? app->GetTime().GetFrameCount() : 0;
		}
	}

	int FontManager::QuantizeBakeSize(float pixelSize) {
		const int p = std::max(1, static_cast<int>(std::lround(pixelSize)));
		auto snap = [](int v, int step) { return ((v + step / 2) / step) * step; };
		if (s_BakeStep > 0) {
			// Fixed step: snap to the nearest multiple, but never below one step
			// (snap() can round small sizes down to 0).
			return std::max(s_BakeStep, snap(p, s_BakeStep));
		}
		// Adaptive ladder: fine for small text, coarse for large.
		if (p <= 16)  return p;
		if (p <= 32)  return snap(p, 2);
		if (p <= 64)  return snap(p, 4);
		if (p <= 128) return snap(p, 8);
		return snap(p, 16);
	}

	void FontManager::SetBakeStep(int step) {
		const int normalized = step < 0 ? 0 : step;
		if (normalized == s_BakeStep) return;
		s_BakeStep = normalized;

		// Re-key existing slots under the new quantization so a later resolve hits
		// them instead of baking a duplicate atlas for the same pixel size. If two
		// old buckets now collapse to one, the most recent slot wins the lookup and
		// the orphan is reclaimed by the LRU. (No-op before Initialize: s_Slots empty.)
		s_Lookup.clear();
		for (uint16_t i = 0; i < static_cast<uint16_t>(s_Slots.size()); ++i) {
			Slot& slot = s_Slots[i];
			if (!slot.InUse || slot.AssetUUID == 0) continue;
			slot.QuantizedSize = QuantizeBakeSize(slot.PixelSize);
			s_Lookup[LookupKey{ slot.AssetUUID, slot.QuantizedSize }] = i;
		}
	}

	void FontManager::SetAtlasMemoryBudgetMB(int megabytes) {
		s_AtlasBudgetBytes = megabytes <= 0
			? 0
			: static_cast<size_t>(megabytes) * 1024 * 1024;
	}

	int FontManager::GetAtlasMemoryBudgetMB() {
		return static_cast<int>(s_AtlasBudgetBytes / (1024 * 1024));
	}

	std::shared_ptr<const std::vector<uint8_t>> FontManager::GetSharedTtf(
		uint64_t assetId, const std::string& path)
	{
		// Reuse the cached bytes only if they came from the same path — a moved/
		// renamed font keeps its UUID but resolves to a new path, so a mismatch
		// must re-read. (In-place edits to the same path during an editor session
		// still serve cached bytes; fonts aren't hot-reload-watched like textures.)
		auto it = s_TtfBufferCache.find(assetId);
		if (it != s_TtfBufferCache.end() && it->second.Bytes && it->second.Path == path) {
			return it->second.Bytes;
		}
		std::vector<uint8_t> bytes = File::ReadAllBytes(path);
		if (bytes.empty()) {
			IDX_CORE_ERROR_TAG("FontManager", "TTF read failed: {}", path);
			return nullptr;
		}
		auto shared = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
		s_TtfBufferCache[assetId] = TtfBufferEntry{ shared, path };
		return shared;
	}

	bool FontManager::Initialize() {
		if (s_IsInitialized) return true;

		s_Slots.clear();
		s_Lookup.clear();
		s_DefaultFont = FontHandle::Invalid();
		s_IsInitialized = true;

		// Same default-font registration the OpenGL impl does — keeps the
		// inspector's font picker populated for projects that ship none.
		std::string fontDir = Path::ResolveIndexAssets("Fonts");
		if (fontDir.empty()) {
			fontDir = Path::Combine(Path::ExecutableDir(), "IndexAssets", "Fonts");
		}
		const std::string defaultFontPath = Path::Combine(fontDir, "GoogleSans-Regular.ttf");
		if (std::filesystem::exists(defaultFontPath)) {
			std::error_code ec;
			const std::string canonical = std::filesystem::weakly_canonical(
				std::filesystem::path(defaultFontPath), ec).make_preferred().string();
			AssetRegistry::RegisterBuiltInAsset(
				ec ? defaultFontPath : canonical, k_DefaultFontAssetId, AssetKind::Font);
			IDX_CORE_INFO_TAG("FontManager", "Default font registered: {}",
				ec ? defaultFontPath : canonical);
		}
		else {
			IDX_CORE_WARN_TAG("FontManager", "Default font missing: {}", defaultFontPath);
		}
		return true;
	}

	void FontManager::Shutdown() {
		if (!s_IsInitialized) return;
		UnloadAll();
		s_TtfBufferCache.clear();
		s_DefaultFont = FontHandle::Invalid();
		s_IsInitialized = false;
	}

	FontHandle FontManager::LoadFont(std::string_view path, float pixelSize) {
		if (!s_IsInitialized || path.empty() || pixelSize <= 0.0f) {
			return FontHandle::Invalid();
		}
		const std::string pathStr(path);
		const uint64_t uuid = AssetRegistry::GetOrCreateAssetUUID(pathStr);
		if (uuid != 0) {
			return LoadFontByUUID(uuid, pixelSize);
		}
		// Fall through: load directly from path with a synthetic UUID
		// so the slot still has a stable key.
		auto font = std::make_unique<Font>();
		if (!font->LoadFromFile(pathStr, pixelSize)) {
			return FontHandle::Invalid();
		}
		return CreateSlot(std::move(font), 0, pixelSize);
	}

	FontHandle FontManager::LoadFontByUUID(uint64_t assetId, float pixelSize) {
		if (!s_IsInitialized || assetId == 0 || pixelSize <= 0.0f) {
			return FontHandle::Invalid();
		}

		FontHandle existing = FindExisting(assetId, pixelSize);
		if (existing.IsValid()) return existing;

		// ResolvePath self-recovers once on a miss; don't MarkDirty()+Sync() (clears the negative
		// cache → full O(N) Assets/ rescan ×3 per dangling GUID, re-arms every other miss).
		const std::string path = AssetRegistry::ResolvePath(assetId);
		if (path.empty()) return FontHandle::Invalid();

		std::shared_ptr<const std::vector<uint8_t>> ttf = GetSharedTtf(assetId, path);
		if (!ttf) return FontHandle::Invalid();

		auto font = std::make_unique<Font>();
		if (!font->LoadFromBuffer(path, std::move(ttf), pixelSize)) {
			return FontHandle::Invalid();
		}
		return CreateSlot(std::move(font), assetId, pixelSize);
	}

	FontHandle FontManager::LoadFontByUUIDAsync(uint64_t assetId, float pixelSize) {
		if (!s_IsInitialized || assetId == 0 || pixelSize <= 0.0f) {
			return FontHandle::Invalid();
		}

		// Uses raw slot lookup (not FindExisting) so a second async request for the same key latches onto the in-flight bake.
		const LookupKey key{ assetId, QuantizeBakeSize(pixelSize) };
		auto it = s_Lookup.find(key);
		if (it != s_Lookup.end()) {
			const uint16_t idx = it->second;
			if (idx < s_Slots.size() && s_Slots[idx].InUse) {
				return FontHandle{ idx, s_Slots[idx].Generation };
			}
		}

		// ResolvePath self-recovers once on a miss; don't MarkDirty()+Sync() (clears the negative
		// cache → full O(N) Assets/ rescan ×3 per dangling GUID, re-arms every other miss).
		const std::string path = AssetRegistry::ResolvePath(assetId);
		if (path.empty()) return FontHandle::Invalid();

		std::shared_ptr<const std::vector<uint8_t>> ttf = GetSharedTtf(assetId, path);
		if (!ttf) return FontHandle::Invalid();

		auto font = std::make_unique<Font>();
		if (!font->BeginAsyncBake(path, std::move(ttf), pixelSize)) {
			return FontHandle::Invalid();
		}
		return CreateSlot(std::move(font), assetId, pixelSize);
	}

	void FontManager::PollAsync() {
		if (!s_IsInitialized) return;
		const int frame = CurrentFrame();
		for (Slot& slot : s_Slots) {
			if (!slot.InUse || !slot.Font) continue;
			if (!slot.Font->IsBakingAsync()) continue;
			slot.Font->PollAsyncBake();
			// Just-published atlases haven't been GetFont'd yet this frame; stamp
			// them so the eviction pass below can't reclaim them the instant they bake.
			if (slot.Font->IsLoaded()) {
				slot.LastAccessFrame = frame;
				slot.LastAccessTick = ++s_AccessTick;
			}
		}
		// A bake may have just published a new atlas; reclaim old ones if over budget.
		EnforceMemoryBudget();
	}

	void FontManager::UnloadFont(const FontHandle& handle) {
		if (!IsValid(handle)) return;
		Slot& slot = s_Slots[handle.index];
		// Use the bucket the slot was registered under, not a recomputed one —
		// SetBakeStep may have changed the quantization since this slot was created.
		const LookupKey key{ slot.AssetUUID, slot.QuantizedSize };
		auto it = s_Lookup.find(key);
		if (it != s_Lookup.end() && it->second == handle.index) {
			s_Lookup.erase(it);
		}
		slot.Font.reset();
		slot.AssetUUID = 0;
		slot.PixelSize = 0.0f;
		slot.InUse = false;
		++slot.Generation;
	}

	void FontManager::UnloadAll() {
		for (Slot& slot : s_Slots) {
			if (slot.InUse) {
				slot.Font.reset();
				slot.AssetUUID = 0;
				slot.PixelSize = 0.0f;
				slot.InUse = false;
				++slot.Generation;
			}
		}
		s_Lookup.clear();
		s_DefaultFont = FontHandle::Invalid();
	}

	void FontManager::EnforceMemoryBudget() {
		if (s_AtlasBudgetBytes == 0) return;  // unlimited

		size_t total = 0;
		for (const Slot& s : s_Slots) {
			if (s.InUse && s.Font && s.Font->IsLoaded()) {
				total += s.Font->GetAtlasByteSize();
			}
		}
		if (total <= s_AtlasBudgetBytes) return;

		const uint16_t defaultIdx = s_DefaultFont.IsValid()
			? s_DefaultFont.index : FontHandle::k_InvalidIndex;
		const int currentFrame = CurrentFrame();

		// Evict least-recently-used loaded atlases until under budget. The current
		// frame's (and previous frame's) working set is protected: if the live set
		// alone exceeds the budget we stay over rather than evict-and-rebake what's
		// about to be drawn again — that would thrash the worker + GPU every frame.
		// A stale handle held by a TextRendererComponent self-heals: IsValid() fails
		// next frame → it re-resolves and re-bakes, exactly like a scene reload.
		while (total > s_AtlasBudgetBytes) {
			int victim = -1;
			uint64_t oldest = std::numeric_limits<uint64_t>::max();
			for (size_t i = 0; i < s_Slots.size(); ++i) {
				const Slot& s = s_Slots[i];
				if (!s.InUse || !s.Font || !s.Font->IsLoaded()) continue;  // skip free + in-flight bakes
				if (static_cast<uint16_t>(i) == defaultIdx) continue;      // never evict the fallback font
				if (currentFrame - s.LastAccessFrame <= 1) continue;       // protect this/last frame's working set
				if (s.LastAccessTick < oldest) {
					oldest = s.LastAccessTick;
					victim = static_cast<int>(i);
				}
			}
			if (victim < 0) break;  // only the live working set remains; stay over budget

			const size_t freed = s_Slots[victim].Font->GetAtlasByteSize();
			UnloadFont(FontHandle{ static_cast<uint16_t>(victim), s_Slots[victim].Generation });
			if (freed >= total) break;  // freed was a summand of total; guards the subtraction
			total -= freed;
		}
	}

	bool FontManager::IsValid(const FontHandle& handle) {
		if (!s_IsInitialized) return false;
		if (!handle.IsValid()) return false;
		if (handle.index >= s_Slots.size()) return false;
		const Slot& slot = s_Slots[handle.index];
		return slot.InUse
			&& slot.Generation == handle.generation
			&& slot.Font
			&& slot.Font->IsLoaded();
	}

	Font* FontManager::GetFont(const FontHandle& handle) {
		if (!IsValid(handle)) return nullptr;
		Slot& slot = s_Slots[handle.index];
		slot.LastAccessTick = ++s_AccessTick;       // most-recently-used for the LRU
		slot.LastAccessFrame = CurrentFrame();      // mark as part of this frame's working set
		return slot.Font.get();
	}

	uint64_t FontManager::GetFontAssetUUID(const FontHandle& handle) {
		if (!IsValid(handle)) return 0;
		return s_Slots[handle.index].AssetUUID;
	}

	FontHandle FontManager::GetDefaultFont() {
		if (!s_IsInitialized) return FontHandle::Invalid();
		if (IsValid(s_DefaultFont)) return s_DefaultFont;
		s_DefaultFont = LoadFontByUUID(k_DefaultFontAssetId, 32.0f);
		return s_DefaultFont;
	}

	FontHandle FontManager::CreateSlot(std::unique_ptr<Font> font, uint64_t assetUUID, float pixelSize) {
		if (!font) return FontHandle::Invalid();
		// Reuse a free slot if one exists, else append.
		uint16_t idx = static_cast<uint16_t>(s_Slots.size());
		for (size_t i = 0; i < s_Slots.size(); ++i) {
			if (!s_Slots[i].InUse) { idx = static_cast<uint16_t>(i); break; }
		}
		if (idx == s_Slots.size()) {
			s_Slots.emplace_back();
		}
		Slot& slot = s_Slots[idx];
		slot.Font = std::move(font);
		slot.AssetUUID = assetUUID;
		slot.PixelSize = pixelSize;
		slot.QuantizedSize = QuantizeBakeSize(pixelSize);
		slot.LastAccessTick = ++s_AccessTick;
		slot.LastAccessFrame = CurrentFrame();
		slot.InUse = true;
		FontHandle h{ idx, slot.Generation };

		if (assetUUID != 0) {
			s_Lookup[LookupKey{ assetUUID, slot.QuantizedSize }] = idx;
		}
		return h;
	}

	FontHandle FontManager::FindExisting(uint64_t assetUUID, float pixelSize) {
		auto it = s_Lookup.find(LookupKey{ assetUUID, QuantizeBakeSize(pixelSize) });
		if (it == s_Lookup.end()) return FontHandle::Invalid();
		const uint16_t idx = it->second;
		if (idx >= s_Slots.size() || !s_Slots[idx].InUse) {
			return FontHandle::Invalid();
		}
		// Return Invalid for in-flight bakes so sync callers get a ready font; async callers use their own probe.
		if (s_Slots[idx].Font && !s_Slots[idx].Font->IsLoaded()) {
			return FontHandle::Invalid();
		}
		return FontHandle{ idx, s_Slots[idx].Generation };
	}

} // namespace Index
