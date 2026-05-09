#pragma once
#include "Collections/Vec2.hpp"
#include "Collections/Color.hpp"
#include "Graphics/TextureHandle.hpp"

namespace Axiom {
    struct Instance44 {
        Vec2 Position{};
        Vec2 Scale{ 1.0f, 1.0f };
        float Rotation{ 0.0f };
        Color Color{};
        TextureHandle TextureHandle{};
        short SortingOrder{ 0 };
        std::uint8_t SortingLayer{ 0 };
        // CPU-only hierarchy walk index used by GuiRenderer as the
        // tiebreaker after (SortingLayer, SortingOrder). Lower = drawn
        // earlier (further back). Not read by any shader — it just sits
        // in the per-instance VBO and is ignored by GL via offsetof'd
        // attribute bindings. Default 0 is fine for non-UI callers like
        // SpriteRenderer that don't care about hierarchy ordering.
        std::uint32_t DrawIndex{ 0 };

        // CPU-only clip rect in centered-screen-space pixels, used by
        // GuiRenderer to drive glScissor when this instance sits under
        // a UI Mask ancestor. HasClip=false means "no clip; render
        // unrestricted". Same offsetof-bound-attribute story as
        // DrawIndex — these fields are never read by any shader.
        bool HasClip{ false };
        Vec2 ClipMin{};
        Vec2 ClipMax{};

        Instance44(Vec2 pos,
            Vec2 scale,
            float rotation,
            Axiom::Color color,
            Axiom::TextureHandle tex,
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