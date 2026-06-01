#pragma once
#include "Core/Export.hpp"

#include <cstdint>
#include <limits>

namespace Index {
    // Stable GUID for the engine-shipped default font. In this header (not FontManager.hpp) so component default-initializers don't drag in stb_truetype.
    inline constexpr uint64_t k_DefaultFontAssetId = 0xAB00000000000001ULL;

    // Slot-table handle (16-bit index + 16-bit generation), same shape as TextureHandle.
    struct INDEX_API FontHandle {
        uint16_t index;
        uint16_t generation;

        static constexpr uint16_t k_InvalidIndex = std::numeric_limits<uint16_t>::max();
        static FontHandle Invalid() { return FontHandle(k_InvalidIndex, 0); }

        FontHandle(uint16_t index, uint16_t generation) : index{ index }, generation{ generation } {}
        FontHandle() : index{ k_InvalidIndex }, generation{ 0 } {}

        bool IsValid() const { return index != k_InvalidIndex; }

        bool operator==(const FontHandle& other) const {
            return index == other.index && generation == other.generation;
        }

        bool operator!=(const FontHandle& other) const {
            return !(*this == other);
        }
    };
}
