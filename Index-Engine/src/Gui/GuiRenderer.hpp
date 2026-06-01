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

namespace Index {
	class Scene;
	class SceneManager;

	class INDEX_API GuiRenderer {
	public:
		GuiRenderer() = default;
		void Initialize();
		void Shutdown();

		void BeginFrame(const SceneManager& sceneManager);
		void EndFrame();

		void RenderScene(const Scene& scene);
		void RenderScene(const Scene& scene, const glm::mat4& worldVP, float pixelToWorldScale);
		static float ComputeWorldUIPixelScale();
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
