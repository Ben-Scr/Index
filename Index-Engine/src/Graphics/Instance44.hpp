#pragma once
#include "Collections/Vec2.hpp"
#include "Collections/Color.hpp"
#include "Graphics/SpriteUVResolver.hpp"
#include "Graphics/TextureHandle.hpp"
#include <cstdint>

namespace Index {
    struct Instance44 {
        Vec2 Position{};
        Vec2 Scale{ 1.0f, 1.0f };
        float Rotation{ 0.0f };
        Color Color{};
        TextureHandle TextureHandle{};
        short SortingOrder{ 0 };
        std::uint8_t SortingLayer{ 0 };
        SpriteUVRect UvRect{};
        // CPU-only GuiRenderer tiebreaker; not read by any shader.
        std::uint32_t DrawIndex{ 0 };

        // CPU-only scissor rect for GuiRenderer UI masking; not read by any shader.
        bool HasClip{ false };
        Vec2 ClipMin{};
        Vec2 ClipMax{};

        // Optional texture-backed UI mask. The scissor rect above still
        // limits raster work; these fields let the sprite shader reject
		// transparent pixels inside the mask texture.
		bool HasTextureMask{ false };
		Index::TextureHandle MaskTextureHandle{};
        SpriteUVRect MaskUvRect{};
        Vec2 MaskRectMin{};
        Vec2 MaskRectMax{};
        Vec2 MaskPivot{};
        float MaskRotation{ 0.0f };

        Instance44(Vec2 pos,
            Vec2 scale,
            float rotation,
            Index::Color color,
            Index::TextureHandle tex,
            short sortingOrder,
            std::uint8_t sortingLayer,
            std::uint32_t drawIndex = 0)
            : Position(pos)
            , Scale(scale)
            , Rotation(rotation)
            , Color(color)
            , TextureHandle(tex)
            , SortingOrder(sortingOrder)
            , SortingLayer(sortingLayer)
            , DrawIndex(drawIndex)
        {
        }

        Instance44() = default;

        Instance44(const Instance44&) = default;
        Instance44& operator=(const Instance44&) = default;
        Instance44(Instance44&&) noexcept = default;
        Instance44& operator=(Instance44&&) noexcept = default;
    };
}
