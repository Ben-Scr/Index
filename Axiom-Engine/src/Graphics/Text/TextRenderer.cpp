#include "pch.hpp"
#include "Graphics/Text/TextRenderer.hpp"

#include "Components/General/Transform2DComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Tags.hpp"
#include "Graphics/Shader.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Scene/Scene.hpp"
#include "Serialization/Path.hpp"

#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstddef>

namespace Axiom {
    namespace {
        constexpr size_t k_VerticesPerGlyph = 6;
        constexpr size_t k_InitialVertexCapacity = 1024;

        // Pixel-space width of a single line at the font's baked size.
        // Used for left/center/right alignment offset. `letterSpacing` is
        // the same screen-pixel-domain value we add per advance step in
        // EmitText so the alignment math stays consistent with what's
        // actually drawn.
        float MeasureLineWidth(const Font& font, std::string_view line, float letterSpacing) {
            float width = 0.0f;
            uint32_t prev = 0;
            int glyphCount = 0;
            for (size_t i = 0; i < line.size(); ++i) {
                const uint32_t cp = static_cast<uint8_t>(line[i]);
                const GlyphMetrics* g = font.GetGlyph(cp);
                if (!g) {
                    prev = 0;
                    continue;
                }
                if (prev != 0) {
                    width += font.GetKerning(prev, cp);
                }
                width += g->XAdvance;
                if (glyphCount > 0) {
                    width += letterSpacing;
                }
                ++glyphCount;
                prev = cp;
            }
            return width;
        }
    }

    Font* TextRenderer::ResolveFont(TextRendererComponent& text) {
        if (FontManager::IsValid(text.ResolvedFont)) {
            Font* font = FontManager::GetFont(text.ResolvedFont);
            if (font) {
                return font;
            }
        }

        const uint64_t uuid = static_cast<uint64_t>(text.FontAssetId);
        if (uuid != 0) {
            text.ResolvedFont = FontManager::LoadFontByUUID(uuid, text.FontSize);
            if (Font* f = FontManager::GetFont(text.ResolvedFont)) {
                return f;
            }
        }

        text.ResolvedFont = FontManager::GetDefaultFont();
        Font* fallback = FontManager::GetFont(text.ResolvedFont);
        if (!fallback) {
            // One-shot: surface the failure in the log instead of silently
            // rendering blank text. Users who see no text would otherwise
            // have no idea whether the renderer is broken or just couldn't
            // find a font asset to draw with.
            static bool s_LoggedMissingFont = false;
            if (!s_LoggedMissingFont) {
                s_LoggedMissingFont = true;
                AIM_CORE_WARN_TAG("TextRenderer",
                    "No font available - assign one in the inspector or ensure AxiomAssets/Fonts/DefaultSans-Regular.ttf is shipped next to the executable.");
            }
        }
        return fallback;
    }

    TextRenderer::TextRenderer() = default;
    TextRenderer::~TextRenderer() {
        Shutdown();
    }

    void TextRenderer::Initialize() {
        if (m_IsInitialized) {
            return;
        }

        std::string shaderDir = Path::ResolveAxiomAssets("Shader");
        if (shaderDir.empty()) {
            shaderDir = Path::Combine(Path::ExecutableDir(), "AxiomAssets", "Shader");
        }

        m_Shader = std::make_unique<Shader>(
            Path::Combine(shaderDir, "2D/text.vert.glsl"),
            Path::Combine(shaderDir, "2D/text.frag.glsl"));
        if (!m_Shader->IsValid()) {
            AIM_CORE_ERROR_TAG("TextRenderer", "Text shader failed to compile — text rendering disabled");
            m_Shader.reset();
            return;
        }

        m_Shader->Submit();
        const GLuint program = m_Shader->GetHandle();
        m_uMVP = glGetUniformLocation(program, "uMVP");
        m_uAtlas = glGetUniformLocation(program, "uAtlas");
        if (m_uAtlas >= 0) {
            glUniform1i(m_uAtlas, 0);
        }
        glUseProgram(0);

        glGenVertexArrays(1, &m_VAO);
        glGenBuffers(1, &m_VBO);

        glBindVertexArray(m_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
        m_VBOCapacity = sizeof(TextVertex) * k_InitialVertexCapacity;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_VBOCapacity), nullptr, GL_DYNAMIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(TextVertex), reinterpret_cast<void*>(offsetof(TextVertex, X)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(TextVertex), reinterpret_cast<void*>(offsetof(TextVertex, U)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(TextVertex), reinterpret_cast<void*>(offsetof(TextVertex, R)));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        m_Vertices.reserve(k_InitialVertexCapacity);
        m_Runs.reserve(16);

        m_IsInitialized = true;
        AIM_CORE_INFO_TAG("TextRenderer", "Text renderer initialized");
    }

    void TextRenderer::Shutdown() {
        if (m_VBO) {
            glDeleteBuffers(1, &m_VBO);
            m_VBO = 0;
        }
        if (m_VAO) {
            glDeleteVertexArrays(1, &m_VAO);
            m_VAO = 0;
        }
        m_VBOCapacity = 0;
        m_Vertices.clear();
        m_Vertices.shrink_to_fit();
        m_Runs.clear();
        m_Runs.shrink_to_fit();
        m_Shader.reset();
        m_uMVP = -1;
        m_uAtlas = -1;
        m_IsInitialized = false;
    }

    void TextRenderer::EnsureGpuCapacity(size_t requiredBytes) {
        if (requiredBytes <= m_VBOCapacity) {
            return;
        }
        size_t newCapacity = m_VBOCapacity == 0 ? sizeof(TextVertex) * k_InitialVertexCapacity : m_VBOCapacity;
        while (newCapacity < requiredBytes) {
            newCapacity *= 2;
        }
        glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(newCapacity), nullptr, GL_DYNAMIC_DRAW);
        m_VBOCapacity = newCapacity;
    }

    void TextRenderer::EmitText(Font& font, const std::string& text,
        float worldX, float worldY,
        float scale, const Color& color,
        TextAlignment alignment, float letterSpacing) {

        // Iterate per line so explicit `\n` produces wrapping. Future
        // layout work (auto-wrap, RTL) plugs in here.
        const float lineHeight = font.GetLineHeight() * scale;

        size_t lineStart = 0;
        int lineIndex = 0;
        const size_t textSize = text.size();

        while (lineStart <= textSize) {
            size_t lineEnd = text.find('\n', lineStart);
            if (lineEnd == std::string::npos) {
                lineEnd = textSize;
            }

            std::string_view line(text.data() + lineStart, lineEnd - lineStart);
            const float lineWidth = MeasureLineWidth(font, line, letterSpacing) * scale;

            float alignOffset = 0.0f;
            switch (alignment) {
            case TextAlignment::Center: alignOffset = -lineWidth * 0.5f; break;
            case TextAlignment::Right:  alignOffset = -lineWidth; break;
            case TextAlignment::Left:
            default:                    alignOffset = 0.0f; break;
            }

            // Pen starts at the entity's transform with the requested
            // alignment offset on X. Y advances downward by line index;
            // baseline math is handled inside the per-glyph emit.
            float penX = worldX + alignOffset;
            const float baselineY = worldY - static_cast<float>(lineIndex) * lineHeight;

            uint32_t prev = 0;
            int glyphsOnLine = 0;
            for (size_t i = 0; i < line.size(); ++i) {
                const uint32_t cp = static_cast<uint8_t>(line[i]);
                const GlyphMetrics* g = font.GetGlyph(cp);
                if (!g) {
                    prev = 0;
                    continue;
                }
                if (prev != 0) {
                    penX += font.GetKerning(prev, cp) * scale;
                }
                // Letter spacing applies *between* glyphs only (after the
                // first), so leading/trailing whitespace stays intact and
                // alignment math (which uses the same rule) lines up.
                if (glyphsOnLine > 0) {
                    penX += letterSpacing * scale;
                }

                if (g->Width > 0.0f && g->Height > 0.0f) {
                    // Glyph quad in world space. stb's yoff is positive
                    // *downward* from the baseline (screen-Y-down); we flip
                    // by subtracting so y0 = top of glyph (higher world Y)
                    // and y1 = bottom (lower world Y).
                    const float x0 = penX + g->XOffset * scale;
                    const float y0 = baselineY - g->YOffset * scale;
                    const float x1 = x0 + g->Width * scale;
                    const float y1 = y0 - g->Height * scale;

                    // Two triangles, wound CCW from the camera's view so
                    // the engine-wide GL_BACK cull doesn't drop them.
                    // Sprites use the same BL → BR → TR convention.
                    TextVertex vTL{ x0, y0, g->U0, g->V0, color.r, color.g, color.b, color.a };
                    TextVertex vTR{ x1, y0, g->U1, g->V0, color.r, color.g, color.b, color.a };
                    TextVertex vBR{ x1, y1, g->U1, g->V1, color.r, color.g, color.b, color.a };
                    TextVertex vBL{ x0, y1, g->U0, g->V1, color.r, color.g, color.b, color.a };

                    m_Vertices.push_back(vBL);
                    m_Vertices.push_back(vBR);
                    m_Vertices.push_back(vTR);
                    m_Vertices.push_back(vBL);
                    m_Vertices.push_back(vTR);
                    m_Vertices.push_back(vTL);
                }

                penX += g->XAdvance * scale;
                ++glyphsOnLine;
                prev = cp;
            }

            if (lineEnd == textSize) break;
            lineStart = lineEnd + 1;
            ++lineIndex;
        }
    }

    void TextRenderer::RenderInstances(std::span<const TextDrawCmd> commands, const glm::mat4& mvp) {
        m_LastFrameGlyphCount = 0;
        m_LastFrameDrawCalls = 0;
        if (!m_IsInitialized || !m_Shader || !m_Shader->IsValid() || commands.empty()) {
            return;
        }

        m_Vertices.clear();
        m_Runs.clear();

        // Sort indices into commands rather than the input span (caller
        // owns it and may need it intact for later use).
        std::vector<size_t> order;
        order.reserve(commands.size());
        for (size_t i = 0; i < commands.size(); ++i) {
            if (commands[i].FontPtr && !commands[i].Text.empty()) {
                order.push_back(i);
            }
        }
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const auto& ca = commands[a];
            const auto& cb = commands[b];
            if (ca.SortingLayer != cb.SortingLayer) return ca.SortingLayer < cb.SortingLayer;
            if (ca.SortingOrder != cb.SortingOrder) return ca.SortingOrder < cb.SortingOrder;
            return ca.FontPtr < cb.FontPtr;
        });

        for (size_t i = 0; i < order.size(); ) {
            const TextDrawCmd& head = commands[order[i]];
            GlyphRun run;
            run.Key.AtlasTexture = head.FontPtr->GetAtlasTexture();
            run.Key.SortingOrder = head.SortingOrder;
            run.Key.SortingLayer = head.SortingLayer;
            run.VertexStart = m_Vertices.size();

            size_t j = i;
            while (j < order.size()) {
                const TextDrawCmd& cmd = commands[order[j]];
                if (cmd.FontPtr->GetAtlasTexture() != run.Key.AtlasTexture
                    || cmd.SortingOrder != run.Key.SortingOrder
                    || cmd.SortingLayer != run.Key.SortingLayer)
                {
                    break;
                }
                // EmitText still expects std::string&; build a temporary
                // backed by string_view's contents. Cheap because EmitText
                // doesn't keep a reference past the call.
                std::string buffer(cmd.Text);
                EmitText(*cmd.FontPtr, buffer, cmd.X, cmd.Y, cmd.Scale,
                    cmd.Tint, cmd.Align, cmd.LetterSpacing);
                ++j;
            }

            run.VertexCount = m_Vertices.size() - run.VertexStart;
            if (run.VertexCount > 0) {
                m_Runs.push_back(run);
            }
            i = j;
        }

        if (m_Vertices.empty()) {
            return;
        }

        m_Shader->Submit();
        if (m_uMVP >= 0) {
            glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, glm::value_ptr(mvp));
        }

        glBindVertexArray(m_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_VBO);

        const size_t requiredBytes = m_Vertices.size() * sizeof(TextVertex);
        EnsureGpuCapacity(requiredBytes);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(requiredBytes), m_Vertices.data());

        GLboolean prevBlend = glIsEnabled(GL_BLEND);
        GLint prevBlendSrcRgb = GL_ONE;
        GLint prevBlendDstRgb = GL_ZERO;
        GLint prevBlendSrcAlpha = GL_ONE;
        GLint prevBlendDstAlpha = GL_ZERO;
        GLint prevBlendEquationRgb = GL_FUNC_ADD;
        GLint prevBlendEquationAlpha = GL_FUNC_ADD;
        glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevBlendEquationRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevBlendEquationAlpha);
        if (!prevBlend) glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Text always rides on top of opaque sprites; disable depth so
        // multi-line text doesn't z-fight with itself when the projection
        // squashes glyphs onto the same plane.
        const GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
        if (prevDepth) glDisable(GL_DEPTH_TEST);

        glActiveTexture(GL_TEXTURE0);
        for (const GlyphRun& run : m_Runs) {
            glBindTexture(GL_TEXTURE_2D, run.Key.AtlasTexture);
            glDrawArrays(GL_TRIANGLES,
                static_cast<GLint>(run.VertexStart),
                static_cast<GLsizei>(run.VertexCount));
            ++m_LastFrameDrawCalls;
        }

        glBlendEquationSeparate(prevBlendEquationRgb, prevBlendEquationAlpha);
        glBlendFuncSeparate(prevBlendSrcRgb, prevBlendDstRgb, prevBlendSrcAlpha, prevBlendDstAlpha);
        if (!prevBlend) glDisable(GL_BLEND);
        if (prevDepth) glEnable(GL_DEPTH_TEST);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindVertexArray(0);
        glUseProgram(0);

        m_LastFrameGlyphCount = m_Vertices.size() / k_VerticesPerGlyph;
    }

    void TextRenderer::RenderScene(Scene& scene, const glm::mat4& vp, const AABB& viewportAABB) {
        m_LastFrameGlyphCount = 0;
        m_LastFrameDrawCalls = 0;
        if (!m_IsInitialized || !m_Shader || !m_Shader->IsValid()) {
            return;
        }

        // Reuse m_Pending's capacity across frames — the per-frame text-list size
        // is roughly stable, and the inner clear() keeps capacity intact so we
        // avoid the per-frame heap churn of a fresh local vector.
        m_Pending.clear();

        // Walk all visible TextRendererComponents on entities with a
        // world-space Transform2D (separate from UI text, which lives on
        // RectTransform2D and is rendered by UIRenderer).
        entt::registry& registry = scene.GetRegistry();

        auto view = registry.view<TextRendererComponent, Transform2DComponent>(entt::exclude<DisabledTag>);
        for (auto&& [entity, text, tr] : view.each()) {
            if (text.Text.empty()) continue;

            Font* font = ResolveFont(text);
            if (!font || !font->IsLoaded()) continue;

            // Cull cheaply via a generous AABB around the transform.
            // Tighter bounds (per-line measure) is a future optimization;
            // for typical text sizes this is plenty.
            const AABB approx = AABB::FromTransform(tr);
            if (!AABB::Intersects(viewportAABB, approx)) {
                // Fall back to bigger probe — text may extend beyond
                // the transform's scale-derived AABB.
                const float radius = text.FontSize * static_cast<float>(text.Text.size()) * 0.5f;
                AABB textBounds{
                    { tr.Position.x - radius, tr.Position.y - radius },
                    { tr.Position.x + radius, tr.Position.y + radius }
                };
                if (!AABB::Intersects(viewportAABB, textBounds)) continue;
            }

            const float bakedSize = font->GetPixelSize() > 0.0f ? font->GetPixelSize() : text.FontSize;
            const float drawScale = (text.FontSize / bakedSize)
                * tr.Scale.x
                / k_TextPixelsPerWorldUnit;

            PendingText p;
            p.FontPtr = font;
            p.Text = &text.Text;
            p.WorldX = tr.Position.x;
            p.WorldY = tr.Position.y;
            p.Scale = drawScale;
            p.LetterSpacing = text.LetterSpacing;
            p.Tint = text.Color;
            p.Align = text.HAlign;
            p.SortingOrder = text.SortingOrder;
            p.SortingLayer = text.SortingLayer;
            m_Pending.push_back(p);
        }

        // Build TextDrawCmd list and dispatch.
        std::vector<TextDrawCmd> commands;
        commands.reserve(m_Pending.size());
        for (const PendingText& p : m_Pending) {
            TextDrawCmd cmd;
            cmd.FontPtr = p.FontPtr;
            cmd.Text = std::string_view(*p.Text);
            cmd.X = p.WorldX;
            cmd.Y = p.WorldY;
            cmd.Scale = p.Scale;
            cmd.LetterSpacing = p.LetterSpacing;
            cmd.Tint = p.Tint;
            cmd.Align = p.Align;
            cmd.SortingOrder = p.SortingOrder;
            cmd.SortingLayer = p.SortingLayer;
            commands.push_back(cmd);
        }
        RenderInstances(commands, vp);
    }

}
