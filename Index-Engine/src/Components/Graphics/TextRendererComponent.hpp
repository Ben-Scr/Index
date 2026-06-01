#pragma once
#include "Collections/Color.hpp"
#include "Collections/Vec4.hpp"
#include "Core/Export.hpp"
#include "Core/UUID.hpp"
#include "Graphics/Text/FontHandle.hpp"

#include <cstdint>
#include <string>

namespace Index {

    enum class TextAlignment : uint8_t {
        Left = 0,
        Center,
        Right
    };

    // None = single line + explicit breaks; Word = break at last whitespace; Character = break at overflow glyph.
    enum class TextWrapMode : uint8_t {
        None = 0,
        Word,
        Character
    };

    // 100 matches Unity default: a 32px font on a 1x1 sprite entity stays visually balanced.
    inline constexpr float k_TextPixelsPerWorldUnit = 100.0f;

    // 2D text renderer. FontSize in pixels; renderer divides by k_TextPixelsPerWorldUnit for world-space.
    struct INDEX_API TextRendererComponent {
        std::string Text = "Hello, World!";

        // Defaults to GoogleSans-Regular GUID; without this, UUID() rolls a random id that resolves to nothing and the inspector shows "(Missing Asset)".
        UUID FontAssetId{ k_DefaultFontAssetId };

        // Cached resolved handle — populated by the renderer so the
        // FontManager lookup happens at most once per FontAssetId per
        // frame. Not serialized.
        FontHandle ResolvedFont{};

        // Pixel height the atlas is baked at; also drives display size
        // via the px→world conversion above. Reasonable values: 16-128.
        float FontSize = 32.0f;

        Color Color{ 1.0f, 1.0f, 1.0f, 1.0f };

        float LetterSpacing = 0.0f;

        TextAlignment HAlign = TextAlignment::Left;

        // World-space text (no RectTransform2D) never wraps; UI text wraps inside rect-width minus Margin. WrapWidth field removed — Margin already provides the inset.
        TextWrapMode WrapMode = TextWrapMode::None;

        // Screen-pixel insets: x=left, y=top, z=right (narrows wrap width), w=bottom (reserved). World-space text only honours x/y today.
        Vec4 Margin{ 0.0f, 0.0f, 0.0f, 0.0f };

        // Sort batch — same semantics as SpriteRendererComponent. Text
        // batches independently from sprites today; the sort key only
        // matters within the text batch.
        int16_t SortingOrder = 0;
        uint8_t SortingLayer = 0;
    };

}
