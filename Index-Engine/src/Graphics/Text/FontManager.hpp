#pragma once
#include "Core/Export.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/FontHandle.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Index {

    class INDEX_API FontManager {
    public:
        static bool Initialize();
        static void Shutdown();
        static bool IsInitialized() { return s_IsInitialized; }

        static FontHandle LoadFont(std::string_view path, float pixelSize);
        static FontHandle LoadFontByUUID(uint64_t assetId, float pixelSize);

        // Returns immediately; handle is invalid until the bake publishes. Re-uses an existing slot for the same (uuid, pixelSize) bucket so repeated frame calls don't spawn duplicate workers.
        static FontHandle LoadFontByUUIDAsync(uint64_t assetId, float pixelSize);

        static void PollAsync();

        static void UnloadFont(const FontHandle& handle);
        static void UnloadAll();

        static bool IsValid(const FontHandle& handle);
        static Font* GetFont(const FontHandle& handle);
        static uint64_t GetFontAssetUUID(const FontHandle& handle);

        static FontHandle GetDefaultFont();

        // ── Bake-step config ────────────────────────────────────────────────
        // Single source of truth for size→bucket quantization, shared by the
        // FontManager slot cache and TextRenderer. step == 0 → adaptive ladder
        // (fine for small text, coarse for large); step > 0 → snap every `step` px.
        static void SetBakeStep(int step);
        static int  GetBakeStep() { return s_BakeStep; }
        static int  QuantizeBakeSize(float pixelSize);

        // ── Atlas memory budget (LRU eviction) ──────────────────────────────
        // megabytes == 0 → unlimited (no eviction). Otherwise the least-recently
        // used baked atlases are evicted once total resident atlas bytes exceed it.
        static void SetAtlasMemoryBudgetMB(int megabytes);
        static int  GetAtlasMemoryBudgetMB();
        static void EnforceMemoryBudget();

    private:
        struct Slot {
            std::unique_ptr<Font> Font;
            uint64_t AssetUUID = 0;
            float PixelSize = 0.0f;
            // Quantized bucket this slot was registered under. Stored (not recomputed)
            // so UnloadFont erases the right s_Lookup entry even if SetBakeStep changed
            // the quantization since this slot was created.
            int QuantizedSize = 0;
            // Monotonic stamp updated on every GetFont; smallest = least recently used.
            uint64_t LastAccessTick = 0;
            // Engine frame this slot was last drawn/created. The current frame's
            // (and previous frame's) working set is protected from eviction.
            int LastAccessFrame = 0;
            uint16_t Generation = 0;
            bool InUse = false;
        };

        static FontHandle CreateSlot(std::unique_ptr<Font> font, uint64_t assetUUID, float pixelSize);
        static FontHandle FindExisting(uint64_t assetUUID, float pixelSize);

        // Canonical TTF bytes for an asset, loaded once and shared across every
        // baked size. Returns nullptr if the file can't be read.
        static std::shared_ptr<const std::vector<uint8_t>> GetSharedTtf(uint64_t assetId,
            const std::string& path);

        struct LookupKey {
            uint64_t Uuid = 0;
            int PixelSizeQuantized = 0;
            bool operator==(const LookupKey& o) const noexcept {
                return Uuid == o.Uuid && PixelSizeQuantized == o.PixelSizeQuantized;
            }
        };
        struct LookupKeyHash {
            size_t operator()(const LookupKey& k) const noexcept {
                return std::hash<uint64_t>{}(k.Uuid) ^ (std::hash<int>{}(k.PixelSizeQuantized) << 1);
            }
        };

        inline static bool s_IsInitialized = false;
        inline static std::vector<Slot> s_Slots;
        inline static std::unordered_map<LookupKey, uint16_t, LookupKeyHash> s_Lookup;
        inline static FontHandle s_DefaultFont;

        // Bake-step config (0 = adaptive ladder) and LRU budget (0 = unlimited).
        inline static int s_BakeStep = 0;
        inline static size_t s_AtlasBudgetBytes = static_cast<size_t>(128) * 1024 * 1024;
        inline static uint64_t s_AccessTick = 0;

        struct TtfBufferEntry {
            std::shared_ptr<const std::vector<uint8_t>> Bytes;
            std::string Path;
        };
        // Keyed by asset UUID — one shared TTF copy per font file, reused by all
        // its baked sizes (the dedup that keeps N sizes from holding N file copies).
        inline static std::unordered_map<uint64_t, TtfBufferEntry> s_TtfBufferCache;
    };

}
