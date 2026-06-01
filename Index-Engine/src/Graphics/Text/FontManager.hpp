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

    private:
        struct Slot {
            std::unique_ptr<Font> Font;
            uint64_t AssetUUID = 0;
            float PixelSize = 0.0f;
            uint16_t Generation = 0;
            bool InUse = false;
        };

        static FontHandle CreateSlot(std::unique_ptr<Font> font, uint64_t assetUUID, float pixelSize);
        static FontHandle FindExisting(uint64_t assetUUID, float pixelSize);

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

        struct TtfBufferEntry {
            std::vector<uint8_t> Bytes;
            std::string Path;
        };
        inline static std::unordered_map<uint64_t, TtfBufferEntry> s_TtfBufferCache;
    };

}
