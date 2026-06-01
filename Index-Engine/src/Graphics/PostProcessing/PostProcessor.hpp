#pragma once

#include "Core/Export.hpp"
#include "Graphics/Shader.hpp"

#include <webgpu/webgpu_cpp.h>

#include <memory>

// PostProcessor: sits between Renderer2D and present; reads an RGBA16F FBO and writes caller-supplied swap-chain or editor-panel view. This header pulls in <webgpu/webgpu_cpp.h> because the dst TextureView cannot be round-tripped through a raw pointer.

namespace Index {

	class Framebuffer;
	struct PostProcessing2DComponent;

	class INDEX_API PostProcessor {
	public:
		PostProcessor();
		~PostProcessor();
		PostProcessor(const PostProcessor&) = delete;
		PostProcessor& operator=(const PostProcessor&) = delete;

		bool Initialize();

		// Release Dawn handles. Idempotent.
		void Shutdown();

		bool IsInitialized() const;

		// Opens a RenderPass with LoadOp::Load so a prior clear on dstView is preserved; dst view comes straight from BeginRenderToCurrentTarget (swap chain or editor FBO).
		void Blit(const Framebuffer& src,
			wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			uint32_t dstWidth,
			uint32_t dstHeight);

		// Dispatches to the enabled effect pass or falls through to Blit. settings == nullptr (no PostProcessing2DComponent on camera) is a valid passthrough case.
		void Run(const Framebuffer& src,
			wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			uint32_t dstWidth,
			uint32_t dstHeight,
			const PostProcessing2DComponent* settings);

		// Public only so anonymous-namespace pipeline-cache builders in PostProcessor.cpp can take Impl& params.
		struct Impl;

	private:
		std::unique_ptr<Impl> m_Impl;

		// Owned here (not in pImpl) so Shader dtor runs after pImpl pipelines are destroyed — pImpl pipelines captured the ShaderModule by value at creation time.
		std::unique_ptr<Shader> m_BlitShader;
		std::unique_ptr<Shader> m_VignetteShader;
		std::unique_ptr<Shader> m_ChromaticShader;
		std::unique_ptr<Shader> m_GrainShader;
		std::unique_ptr<Shader> m_LensDistortionShader;
		std::unique_ptr<Shader> m_ColorGradingShader;
		std::unique_ptr<Shader> m_BloomThresholdShader;
		std::unique_ptr<Shader> m_BloomBlurShader;
		std::unique_ptr<Shader> m_BloomCompositeShader;
		std::unique_ptr<Shader> m_PixelatedShader;
	};

} // namespace Index
