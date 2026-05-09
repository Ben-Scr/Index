#pragma once
#include "Core/Export.hpp"
#include "Graphics/SpriteShaderProgram.hpp"
#include "Graphics/QuadMesh.hpp"
#include "Graphics/Instance44.hpp"
#include "Graphics/Text/TextRenderer.hpp"
#include "Scene/EntityHandle.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Axiom {
	class Scene;
	class SceneManager;

	// Screen-space UI renderer. Walks every loaded scene's
	// RectTransform2D entities each frame, rendering:
	//   - Image components (background quads)
	//   - TextRendererComponent on the same entity (widget labels)
	//   - Dropdown popup option lists when DropdownComponent::IsOpen
	//
	// Coordinate system: centered, +Y up, units = framebuffer pixels.
	// The window viewport (NOT the camera viewport) drives layout, so
	// UI doesn't move when the camera scrolls.
	//
	// World-space mode (RenderScene with an explicit VP) is for the
	// editor view: it renders the same UI through a world-space VP so
	// the user can pan/zoom around their UI alongside scene objects,
	// and selection gizmos line up with the rendered widgets.
	class AXIOM_API GuiRenderer {
	public:
		GuiRenderer() = default;
		void Initialize();
		void Shutdown();

		void BeginFrame(const SceneManager& sceneManager);
		void EndFrame();

		// Screen-space render: ortho sized to UIRegion / main viewport,
		// origin at the centre of that rect. Used by the runtime and by
		// the editor's Game View FBO.
		void RenderScene(const Scene& scene);

		// World-space render: resolved UI-pixel coords are scaled by
		// `pixelToWorldScale` and projected through `worldVP`. Used by
		// the editor view so UI behaves like a world object — pans,
		// zooms, and pairs with selection gizmos drawn in world space.
		// Callers compute the scale (typically via
		// ComputeWorldUIPixelScale()) and pass it in explicitly — this
		// keeps the renderer free of implicit lookups into Application /
		// SceneManager / main-camera state, and lets the editor align
		// gizmos by reusing the same value.
		void RenderScene(const Scene& scene, const glm::mat4& worldVP, float pixelToWorldScale);

		// Pixel → world scale that matches the runtime main camera's
		// viewport size to the UI canvas pixel height. Returns 0.01
		// (100 px = 1 world unit) when the main camera or canvas size
		// isn't available yet. This is a pure utility — the renderer
		// doesn't call it; it exists so editor code (gizmo overlays,
		// world-space UI render) can derive a consistent value to feed
		// into RenderScene's `pixelToWorldScale` parameter.
		static float ComputeWorldUIPixelScale();

		// When true, BeginFrame becomes a no-op so an external driver
		// (e.g. the editor's per-FBO render path) can call RenderScene
		// itself with a target framebuffer already bound. Mirrors
		// Renderer2D::SetSkipBeginFrameRender so editor & runtime share
		// the same opt-out mechanism.
		void SetSkipBeginFrameRender(bool skip) { m_SkipBeginFrameRender = skip; }

	private:
		void CollectAndDraw(const Scene& scene, const glm::mat4& mvp,
			float halfW, float halfH);

		SpriteShaderProgram m_SpriteShader;
		QuadMesh m_QuadMesh;

		// Owned text renderer so UI can render labels in screen-space
		// without going through Renderer2D's world-space text pass.
		std::unique_ptr<TextRenderer> m_TextRenderer;

		// Reused across frames so RenderScene doesn't heap-allocate per frame.
		std::vector<Instance44> m_InstancesScratch;
		std::vector<TextDrawCmd> m_TextScratch;
		// Hierarchy walk output (entity, DrawIndex). Cleared at the top of
		// CollectAndDraw and refilled by UIDrawOrder::Build — capacity persists
		// so a stable UI doesn't allocate after the first frame.
		std::vector<std::pair<EntityHandle, int>> m_DrawOrder;
		// Reverse map for the input-field overlay pass — entity → DrawIndex.
		// Same reuse contract: clear() at the top of CollectAndDraw.
		std::unordered_map<EntityHandle, std::uint32_t> m_DrawIndexByEntity;

		bool m_IsInitialized = false;
		bool m_SkipBeginFrameRender = false;
	};
}
