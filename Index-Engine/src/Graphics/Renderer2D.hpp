#pragma once
#include "QuadMesh.hpp"
#include "SpriteShaderProgram.hpp"
#include "TextureHandle.hpp"
#include "Instance44.hpp"
#include "Collections/AABB.hpp"
#include "Core/Export.hpp"
#include "Graphics/Framebuffer.hpp"
#include "Graphics/IRenderer.hpp"
#include "Graphics/PostProcessing/PostProcessor.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <utility>
#include <vector>
#include <functional>

namespace Index {
	class Scene;
	class TextRenderer;
#ifdef INDEX_PROFILER_ENABLED
	class GpuTimer;
#endif

	class INDEX_API Renderer2D : public IRenderer {
	public:
		Renderer2D();
		~Renderer2D() override;
		Renderer2D(const Renderer2D&) = delete;
		Renderer2D& operator=(const Renderer2D&) = delete;

		void Initialize() override;
		void BeginFrame() override;
		void EndFrame() override;
		void Shutdown() override;

		// MUST call after SwapBuffers: GpuTimer needs MapAsync deferred past the Submit to avoid 'used in submit while mapped'.
		void OnAfterPresent();
		static void ClearSceneCache(const Scene* scene);
		void SetEnabled(bool enabled) { m_IsEnabled = enabled; }
		bool IsEnabled() const { return m_IsEnabled; }

		// Master gate for the post-processing chain. The editor flips this off while
		// rendering the Editor View so scene editing isn't tinted by bloom / vignette
		// / etc.; Game View and the runtime leave it on. Restored to true after the
		// Editor-View pass each frame.
		void SetPostProcessingEnabled(bool enabled) { m_PostProcessingEnabled = enabled; }
		bool IsPostProcessingEnabled() const { return m_PostProcessingEnabled; }
		bool IsInitialized() const override { return m_IsInitialized; }

		void SetOutputTarget(unsigned int fboId, int width, int height) {
			m_OutputFboId = fboId;
			m_OutputWidth = width;
			m_OutputHeight = height;
		}

		void RenderScene(Scene& scene);
		void RenderSceneWithVP(Scene& scene, const glm::mat4& vp, const AABB& viewportAABB);

		void SetSkipBeginFrameRender(bool skip) { m_SkipBeginFrameRender = skip; }

		size_t GetRenderedInstancesCount() const { return m_RenderedInstancesCount; }
		size_t GetDrawCallsCount() const { return m_DrawCallsCount; }
		float GetRenderLoopDuration() const { return m_RenderLoopDuration; }

		using SceneProvider = std::function<void(const std::function<void(Scene&)>&)>;
		void SetSceneProvider(SceneProvider provider) { m_SceneProvider = std::move(provider); }

		using InstanceContributor = std::function<void(const Scene& scene, const AABB& viewportAABB, std::vector<Instance44>& outInstances)>;
		// Returns a token for later removal. Tokens are non-zero on success.
		static uint32_t RegisterInstanceContributor(InstanceContributor contributor);
		// No-op on an unknown / already-removed token.
		static void UnregisterInstanceContributor(uint32_t token);

	private:
		void RenderScenes();
		void CollectAndRenderInstances(Scene& scene, const glm::mat4& vp, const AABB& viewportAABB);

		size_t m_RenderedInstancesCount = 0;
		size_t m_DrawCallsCount = 0;
		float m_RenderLoopDuration = 0.0f;
		bool m_IsInitialized = false;
		bool m_IsEnabled = true;
		bool m_PostProcessingEnabled = true;
		bool m_SkipBeginFrameRender = false;

		std::vector<Instance44> m_Instances;

		unsigned int m_OutputFboId = 0;
		int m_OutputWidth = 0;
		int m_OutputHeight = 0;

		QuadMesh m_QuadMesh;
		SpriteShaderProgram m_SpriteShader;

		// Text passes are layered on top of sprites within the same frame
		// — owned here so Renderer2D's frame lifecycle drives them and
		// callers don't need to thread TextRenderer through their own code.
		std::unique_ptr<TextRenderer> m_TextRenderer;

		Framebuffer    m_SceneFbo;
		// Letterbox target: when aspect-lock is active, routes post-effects here then Blits with the sub-rect viewport so bars stay black.
		Framebuffer    m_LetterboxOutputFbo;
		PostProcessor  m_PostProcessor;

		SceneProvider m_SceneProvider;

#ifdef INDEX_PROFILER_ENABLED
		// GPU-side frame timer. Owned by unique_ptr so the .hpp doesn't
		// need to pull in GpuTimer.hpp's GLuint dependency. nullptr when
		// the profiler is stripped (the whole field is removed via #ifdef).
		std::unique_ptr<GpuTimer> m_GpuTimer;
#endif
	};
}
