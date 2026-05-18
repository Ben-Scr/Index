#pragma once

#include "Core/Export.hpp"
#include "Graphics/Shader.hpp"

#include <webgpu/webgpu_cpp.h>

#include <memory>

// =============================================================================
// PostProcessor — fullscreen-pass post-processing stage (WebGPU).
// -----------------------------------------------------------------------------
// Sits between Renderer2D's scene render and the final present. The renderer
// writes sprites into an intermediate RGBA16F FBO; PostProcessor reads that
// FBO and writes the caller's final target (swap chain or editor panel FBO)
// via one or more fullscreen passes.
//
// Phase B (this file): one passthrough Blit pass. Visually identical to
// today — the scene FBO is just copied to the destination. Lays the WebGPU
// plumbing every future effect needs (bind-group layout, sampler, pipeline
// cache, fullscreen-triangle vertex shader).
//
// Future phases register one pipeline per effect (vignette, color grading,
// bloom, ...). They all share the bind-group layout below (sampled source
// + linear sampler) plus per-effect uniforms in binding 2. The Run() entry
// point will iterate over the PostProcessing2DComponent's enabled effects
// and ping-pong between two intermediate FBOs.
//
// This header transitively pulls in <webgpu/webgpu_cpp.h> because the
// destination target's TextureView comes straight through from
// WebGPUBackend::BeginRenderToCurrentTarget() and can't be round-tripped
// through a raw pointer (the swap-chain view isn't in the engine's
// framebuffer pool). Only Renderer2D consumes this header, so the leak
// is local.
// =============================================================================

namespace Index {

	class Framebuffer;
	struct PostProcessing2DComponent;

	class INDEX_API PostProcessor {
	public:
		PostProcessor();
		~PostProcessor();
		PostProcessor(const PostProcessor&) = delete;
		PostProcessor& operator=(const PostProcessor&) = delete;

		// Build the blit shader, bind-group layout, sampler. Idempotent —
		// safe to call multiple times; second call is a no-op. Must be
		// called after the WebGPU device exists (typically from
		// Renderer2D::Initialize()).
		bool Initialize();

		// Release Dawn handles. Idempotent.
		void Shutdown();

		bool IsInitialized() const;

		// Sample the source framebuffer and write to the destination view.
		// The dst view + format come straight from
		// WebGPUBackend::BeginRenderToCurrentTarget so the caller can blit
		// into the swap chain OR a per-panel editor FBO without going
		// through RenderApi::BindFramebuffer.
		//
		// Opens its own RenderPass against dstView with LoadOp::Load so a
		// prior clear on the destination is preserved (the fullscreen
		// triangle then overwrites the visible region with sampled source
		// pixels).
		void Blit(const Framebuffer& src,
			wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			uint32_t dstWidth,
			uint32_t dstHeight);

		// Top-level entry: read `settings`, dispatch to the matching effect
		// pass (or `Blit` if no effects are enabled / settings is null).
		// Renderer2D calls this once per scene render after the sprite pass
		// has written the intermediate FBO.
		//
		// Phase C1 implements only Vignette. Other Enabled effects in
		// `settings` are silently ignored (a one-time warning is emitted at
		// Initialize); subsequent sub-phases (C2-C5 + D) wire them in.
		//
		// `settings` may be nullptr — happens when the active camera entity
		// has no PostProcessing2DComponent. In that case the call collapses
		// to a passthrough Blit.
		void Run(const Framebuffer& src,
			wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			uint32_t dstWidth,
			uint32_t dstHeight,
			const PostProcessing2DComponent* settings);

		// Impl is forward-declared with its full definition living in
		// PostProcessor.cpp. The struct is `public` only so the .cpp's
		// anonymous-namespace helpers (pipeline cache builders) can take
		// `Impl&` parameters; in every other sense it's a private detail.
		struct Impl;

	private:
		std::unique_ptr<Impl> m_Impl;

		// Per-effect shader handles — owned here rather than in the pImpl
		// so the destruction order is well-defined (Shader's dtor frees the
		// wgpu::ShaderModule pool slot; the pImpl's pipelines captured the
		// module by value during creation, so destroying the Shader after
		// the pipelines is safe).
		std::unique_ptr<Shader> m_BlitShader;
		std::unique_ptr<Shader> m_VignetteShader;
		std::unique_ptr<Shader> m_ChromaticShader;
		std::unique_ptr<Shader> m_GrainShader;
		std::unique_ptr<Shader> m_LensDistortionShader;
		std::unique_ptr<Shader> m_ColorGradingShader;
		// Bloom is a 4-pass pipeline (threshold extract -> separable
		// Gaussian H -> separable Gaussian V -> composite scene+bloom).
		// Each step has its own shader so the WGSL stays focused; the
		// orchestration lives in PostProcessor::RunBloomPass.
		std::unique_ptr<Shader> m_BloomThresholdShader;
		std::unique_ptr<Shader> m_BloomBlurShader;
		std::unique_ptr<Shader> m_BloomCompositeShader;
		std::unique_ptr<Shader> m_PixelatedShader;
	};

} // namespace Index
