#pragma once
#include "Graphics/PosColorVertex.hpp"
#include "Graphics/Gizmo.hpp"
#include "Core/Export.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Index {
    class Camera2DComponent;
    class Shader;
    class TextRenderer;

    // Internal upload-format vertex (declared here so the static buffer
    // member can be defined in the .cpp). The class doesn't expose this
    // type — it only ever flows through FlushGizmosImpl.
    struct GizmoUploadVertex {
        float x, y, z;
        float r, g, b, a;
    };

    class INDEX_API GizmoRenderer2D {
    public:
        static bool Initialize();
        static void Shutdown();
        static void OnResize(int w, int h);
        static void BeginFrame(uint16_t viewId = 1);
        static void EndFrame();

        static void RenderWithVP(const glm::mat4& vp, GizmoLayerMask layerMask = GizmoLayerMask::All);

    private:
        // Returned span is backed by FrameArenas::Frame() and is invalidated
        // by the next Application::EndFrame. Do not cache across frames.
        // BuildGeometry → line list (wire + DrawLine); BuildFilledGeometry → triangle list (filled).
        static std::span<const PosColorVertex> BuildGeometry(GizmoLayerMask layerMask);
        static std::span<const PosColorVertex> BuildFilledGeometry(GizmoLayerMask layerMask);
        // Draw the built geometry through `vp`. No fallback to a global
        // camera — if you don't have a VP, you don't have a draw.
        // filled=true uses the triangle-list pipeline + filled vertex buffer; false the line-list path.
        static void FlushGizmosImpl(const glm::mat4& vp,
            std::span<const PosColorVertex> verts, bool filled);
        // World-space gizmo text, drawn on top via a private TextRenderer.
        static void RenderGizmoText(const glm::mat4& vp, GizmoLayerMask layerMask);

        static bool m_IsInitialized;
        static std::unique_ptr<Shader> m_GizmoShader;
        static std::unique_ptr<TextRenderer> m_TextRenderer;
        // 32-bit indices: Gizmo::s_MaxVertices is 100k; uint16_t would silently alias indices 65536+ onto early vertices.
        static std::vector<uint32_t> m_GizmoIndices;
        static std::vector<GizmoUploadVertex> s_UploadBuffer;

        static uint16_t m_GizmoViewId;

        static unsigned int m_VAO;
        static unsigned int m_VBO;
        static unsigned int m_EBO;
        // E18: track persistent VBO/EBO capacity (in bytes) so we can grow
        // the buffer once and use glBufferSubData each frame.
        static std::size_t m_VBOCapacityBytes;
        static std::size_t m_EBOCapacityBytes;
        static int m_uMVP;
    };
}
