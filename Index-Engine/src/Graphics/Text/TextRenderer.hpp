#pragma once
#include "Collections/AABB.hpp"
#include "Collections/Color.hpp"
#include "Collections/Vec2.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Core/Export.hpp"
#include "Graphics/SpriteUVResolver.hpp"
#include "Graphics/Text/FontHandle.hpp"
#include "Graphics/TextureHandle.hpp"

#include <cstdint>
#include <glm/mat4x4.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Index {

    class Scene;
    class Shader;
    class Font;
    struct TextRendererComponent;

    struct TextVertex {
        float X = 0.0f;
        float Y = 0.0f;
        float U = 0.0f;
        float V = 0.0f;
        float R = 1.0f;
        float G = 1.0f;
        float B = 1.0f;
        float A = 1.0f;
        float MaskRect[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
        float MaskRot[4]{ 0.0f, 0.0f, 1.0f, 0.0f };
        float MaskUv[4]{ 0.0f, 0.0f, 1.0f, 1.0f };
    };

    // Scale = (FontSize / atlasBakedSize) / k_TextPixelsPerWorldUnit for entity text; (desiredPixelHeight / atlasBakedSize) for screen-space.
    // Text is a non-owning view; caller must keep the source alive for the duration of RenderInstances.
    struct INDEX_API TextDrawCmd {
        Font* FontPtr = nullptr;
        std::string_view Text;
        float X = 0.0f;
        float Y = 0.0f;
        float Scale = 1.0f;
        float LetterSpacing = 0.0f;
        // Extra per-line advance in baked-atlas pixels (added to the font's line height).
        float LineSpacing = 0.0f;
        Color Tint{};
        TextAlignment Align = TextAlignment::Left;
        // Distributes a multi-line block above/below cmd.Y. Top is the natural
        // "Y is the first baseline" default that single-line callers (gizmos) assume.
        TextVerticalAlignment VAlign = TextVerticalAlignment::Top;
        // WrapWidthPixels in atlas-pixel units (no Scale baked in); 0/negative short-circuits to no-wrap.
        TextWrapMode Wrap = TextWrapMode::None;
        float WrapWidthPixels = 0.0f;
        int16_t SortingOrder = 0;
        uint8_t SortingLayer = 0;
        std::uint32_t DrawIndex = 0; // tiebreaker after (SortingLayer, SortingOrder); lower = drawn first

        bool HasClip = false; // when true GuiRenderer applies glScissor for text under a UI Mask
        Vec2 ClipMin{};
        Vec2 ClipMax{};

        bool HasTextureMask = false;
        TextureHandle MaskTextureHandle{};
        SpriteUVRect MaskUvRect{};
        Vec2 MaskRectMin{};
        Vec2 MaskRectMax{};
        Vec2 MaskPivot{};
        float MaskRotation = 0.0f;

        // EmitText short-circuits the rotation step when zero, keeping the common axis-aligned path fast.
        float Rotation = 0.0f;
        Vec2 Pivot{};
    };

    class INDEX_API TextRenderer {
    public:
        TextRenderer();
        ~TextRenderer();

        TextRenderer(const TextRenderer&) = delete;
        TextRenderer& operator=(const TextRenderer&) = delete;

        // GL setup. Safe to call once a GL context is alive.
        void Initialize();

        void RenderScene(Scene& scene, const glm::mat4& vp, const AABB& viewportAABB);

        // viewId routes text onto the caller's framebuffer (0xFFFF = current view); scissorCache re-applies clip across multi-atlas Mask spans (0xFFFF = no scissor).
        void RenderInstances(std::span<const TextDrawCmd> commands, const glm::mat4& mvp,
            unsigned short viewId = 0xFFFFu,
            unsigned short scissorCache = 0xFFFFu);

        static Font* ResolveFont(TextRendererComponent& text);

        // Bakes the atlas at an explicit pixel size; use for scaled RectTransform text to avoid upscaling a smaller bake.
        static Font* ResolveFontAtPixelSize(TextRendererComponent& text, float pixelSize);

        // Returns size in atlas-pixel units; caller converts to screen pixels via (FontSize / font.GetPixelSize()).
        // lineSpacing widens the multi-line height by (lineCount-1)*lineSpacing.
        static Vec2 MeasureNaturalSize(Font& font, std::string_view text, float letterSpacing,
            float lineSpacing = 0.0f);

        // Visual line byte-ranges [begin,end) for the text under the given wrap settings,
        // identical to what EmitText renders, so caret/selection/hit-test agree with the
        // wrapped rows. wrapWidthPixels is in baked-atlas pixels; pass 0 or WrapMode::None
        // to split on '\n' only.
        static void ComputeVisualLines(Font& font, std::string_view text, float letterSpacing,
        	TextWrapMode wrapMode, float wrapWidthPixels,
        	std::vector<std::pair<size_t, size_t>>& out);

        // Drop GL state. Must run while the GL context is still alive.
        void Shutdown();

        bool IsInitialized() const { return m_IsInitialized; }
        size_t GetGlyphsRenderedLastFrame() const { return m_LastFrameGlyphCount; }
        size_t GetDrawCallsLastFrame() const { return m_LastFrameDrawCalls; }

    private:
        struct GlyphBatchKey {
            unsigned AtlasTexture = 0;
            int16_t SortingOrder = 0;
            uint8_t SortingLayer = 0;
        };

        struct GlyphRun {
            GlyphBatchKey Key{};
            size_t VertexStart = 0;
            size_t VertexCount = 0;
        };

        void EnsureGpuCapacity(size_t requiredBytes);
        void EmitText(Font& font, std::string_view text,
            float worldX, float worldY,
            float scale, const Color& color,
            TextAlignment alignment, TextVerticalAlignment verticalAlignment,
            float letterSpacing, float lineSpacing,
            TextWrapMode wrapMode = TextWrapMode::None,
            float wrapWidthPixels = 0.0f,
            float rotation = 0.0f, Vec2 pivot = Vec2{ 0.0f, 0.0f },
            bool hasTextureMask = false,
            TextureHandle maskTextureHandle = TextureHandle{},
            SpriteUVRect maskUvRect = SpriteUVRect{},
            Vec2 maskRectMin = Vec2{ 0.0f, 0.0f },
            Vec2 maskRectMax = Vec2{ 0.0f, 0.0f },
            Vec2 maskPivot = Vec2{ 0.0f, 0.0f },
            float maskRotation = 0.0f);

        bool m_IsInitialized = false;

        unsigned m_VAO = 0;
        unsigned m_VBO = 0;
        size_t m_VBOCapacity = 0;

        // Reused across frames so we don't allocate in the hot path.
        std::vector<TextVertex> m_Vertices;
        std::vector<GlyphRun> m_Runs;
        // Visual-line ranges produced by EmitText's wrap pass. (begin,
        // end) byte-offsets into the source string; persists across
        // calls so the wrap loop doesn't churn the heap.
        std::vector<std::pair<size_t, size_t>> m_WrapScratch;
        std::vector<TextDrawCmd> m_PendingDrawCmds; // capacity persists across frames
        std::vector<size_t> m_Order;

        std::unique_ptr<Shader> m_Shader;
        int m_uMVP = -1;
        int m_uAtlas = -1;

        size_t m_LastFrameGlyphCount = 0;
        size_t m_LastFrameDrawCalls = 0;
    };

}
