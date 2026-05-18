#include "pch.hpp"
#include "Graphics/PostProcessing/PostProcessor.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/Framebuffer.hpp"

#include <unordered_map>

// =============================================================================
// PostProcessor — WebGPU (Dawn) implementation.
// -----------------------------------------------------------------------------
// Shader handle, bind-group layout, pipeline layout, sampler, and the
// per-target-format pipeline cache. Initialize() builds everything that
// doesn't depend on the destination format; the per-format pipeline is
// lazily built the first time a given format shows up at Blit().
//
// The blit shader is the embedded "postblit" entry in Shader.cpp's
// k_BuiltIns registry — a single module with vs_main + fs_main that
// generates a fullscreen triangle from vertex_index. No vertex buffer is
// bound; the pipeline declares zero vertex buffers and the draw call
// passes 3 vertices.
// =============================================================================

namespace Index {

	struct PostProcessor::Impl {
		wgpu::BindGroupLayout BindGroupLayout;
		wgpu::PipelineLayout  PipelineLayout;
		wgpu::Sampler         LinearSampler;
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> BlitPipelines;
		bool                  Initialized = false;
	};

	PostProcessor::PostProcessor() : m_Impl(std::make_unique<Impl>()) {}

	PostProcessor::~PostProcessor() {
		Shutdown();
	}

	bool PostProcessor::IsInitialized() const {
		return m_Impl && m_Impl->Initialized;
	}

	bool PostProcessor::Initialize() {
		if (!m_Impl) m_Impl = std::make_unique<Impl>();
		if (m_Impl->Initialized) return true;

		if (!WebGPUBackend::IsInitialized()) {
			IDX_CORE_ERROR_TAG("PostProcessor",
				"Initialize called before WebGPU backend is up");
			return false;
		}

		wgpu::Device device = WebGPUBackend::GetDevice();
		if (!device) {
			IDX_CORE_ERROR_TAG("PostProcessor", "WebGPU device handle null");
			return false;
		}

		// Compile/look up the blit shader via the standard Shader wrapper.
		// The vsPath stem "postblit" routes to k_PostBlitWGSL in Shader.cpp's
		// built-in registry; fsPath is ignored under WebGPU.
		m_BlitShader = std::make_unique<Shader>("postblit", "postblit");
		if (!m_BlitShader->IsValid()) {
			IDX_CORE_ERROR_TAG("PostProcessor",
				"Failed to compile post-blit shader (built-in lookup miss)");
			m_BlitShader.reset();
			return false;
		}

		// Bind group layout — binding 0: sampled source texture (Float),
		// binding 1: filtering sampler. Future effect pipelines will reuse
		// this same layout plus add a binding 2 for per-effect uniforms.
		wgpu::BindGroupLayoutEntry bglEntries[2]{};
		bglEntries[0].binding = 0;
		bglEntries[0].visibility = wgpu::ShaderStage::Fragment;
		bglEntries[0].texture.sampleType = wgpu::TextureSampleType::Float;
		bglEntries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
		bglEntries[0].texture.multisampled = false;

		bglEntries[1].binding = 1;
		bglEntries[1].visibility = wgpu::ShaderStage::Fragment;
		bglEntries[1].sampler.type = wgpu::SamplerBindingType::Filtering;

		wgpu::BindGroupLayoutDescriptor bglDesc{};
		bglDesc.label = "postprocess-blit-bgl";
		bglDesc.entryCount = 2;
		bglDesc.entries = bglEntries;
		m_Impl->BindGroupLayout = device.CreateBindGroupLayout(&bglDesc);
		if (!m_Impl->BindGroupLayout) {
			IDX_CORE_ERROR_TAG("PostProcessor", "CreateBindGroupLayout failed");
			return false;
		}

		wgpu::PipelineLayoutDescriptor plDesc{};
		plDesc.label = "postprocess-blit-pipeline-layout";
		plDesc.bindGroupLayoutCount = 1;
		plDesc.bindGroupLayouts = &m_Impl->BindGroupLayout;
		m_Impl->PipelineLayout = device.CreatePipelineLayout(&plDesc);
		if (!m_Impl->PipelineLayout) {
			IDX_CORE_ERROR_TAG("PostProcessor", "CreatePipelineLayout failed");
			return false;
		}

		// Linear sampler with Clamp-to-edge addressing. Linear filtering is
		// the right default for blit / vignette / color grading; bloom's
		// downsample/upsample passes will reuse this sampler. Clamp prevents
		// the bloom blur's edge taps from wrapping around to the other side
		// of the FBO.
		wgpu::SamplerDescriptor samplerDesc{};
		samplerDesc.label = "postprocess-linear-sampler";
		samplerDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
		samplerDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
		samplerDesc.addressModeW = wgpu::AddressMode::ClampToEdge;
		samplerDesc.magFilter = wgpu::FilterMode::Linear;
		samplerDesc.minFilter = wgpu::FilterMode::Linear;
		samplerDesc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
		samplerDesc.lodMinClamp = 0.0f;
		samplerDesc.lodMaxClamp = 1.0f;
		samplerDesc.maxAnisotropy = 1;
		m_Impl->LinearSampler = device.CreateSampler(&samplerDesc);
		if (!m_Impl->LinearSampler) {
			IDX_CORE_ERROR_TAG("PostProcessor", "CreateSampler failed");
			return false;
		}

		m_Impl->Initialized = true;
		return true;
	}

	void PostProcessor::Shutdown() {
		if (!m_Impl) return;
		m_Impl->BlitPipelines.clear();
		m_Impl->LinearSampler = nullptr;
		m_Impl->PipelineLayout = nullptr;
		m_Impl->BindGroupLayout = nullptr;
		m_Impl->Initialized = false;
		m_BlitShader.reset();
	}

	namespace {
		// Per-target-format pipeline cache. The blit shader is format-agnostic
		// in its WGSL but the wgpu::RenderPipeline bakes the color target
		// format into the pipeline state; one cache entry per (dstFormat).
		wgpu::RenderPipeline GetOrCreateBlitPipeline(
			PostProcessor::Impl& impl,
			const Shader& blitShader,
			wgpu::TextureFormat dstFormat)
		{
			auto it = impl.BlitPipelines.find(dstFormat);
			if (it != impl.BlitPipelines.end()) return it->second;

			wgpu::Device device = WebGPUBackend::GetDevice();
			if (!device) return nullptr;

			WebGPUBackend::ShaderLookup look = WebGPUBackend::LookupShader(blitShader.GetHandle());
			if (!look.Valid) {
				IDX_CORE_ERROR_TAG("PostProcessor",
					"Blit shader module not resolvable (handle={})", blitShader.GetHandle());
				return nullptr;
			}

			// Vertex stage — no vertex buffer; vs_main builds positions from
			// @builtin(vertex_index) for a fullscreen triangle.
			wgpu::VertexState vertexState{};
			vertexState.module = look.Module;
			vertexState.entryPoint = "vs_main";
			vertexState.bufferCount = 0;
			vertexState.buffers = nullptr;

			// Fragment stage — output to the requested target format with
			// straight-alpha blending so the intermediate's alpha channel
			// drives compositing. The filled-pass intermediate is fully
			// opaque (alpha=1 everywhere) so straight-alpha collapses to
			// overwrite, matching the previous behaviour. The wireframe
			// pass clears its intermediate to alpha=0 and writes opaque
			// black at line pixels only — alpha-aware blit composites
			// those edge pixels onto the caller while preserving whatever
			// the caller already painted underneath (filled sprite + UI +
			// gizmos in Mixed draw mode).
			wgpu::BlendState blend{};
			blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
			blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
			blend.color.operation = wgpu::BlendOperation::Add;
			blend.alpha.srcFactor = wgpu::BlendFactor::One;
			blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
			blend.alpha.operation = wgpu::BlendOperation::Add;

			wgpu::ColorTargetState colorTarget{};
			colorTarget.format = dstFormat;
			colorTarget.blend = &blend;
			colorTarget.writeMask = wgpu::ColorWriteMask::All;

			wgpu::FragmentState fragmentState{};
			fragmentState.module = look.Module;
			fragmentState.entryPoint = "fs_main";
			fragmentState.targetCount = 1;
			fragmentState.targets = &colorTarget;

			wgpu::RenderPipelineDescriptor pipelineDesc{};
			pipelineDesc.label = "postprocess-blit-pipeline";
			pipelineDesc.layout = impl.PipelineLayout;
			pipelineDesc.vertex = vertexState;
			pipelineDesc.fragment = &fragmentState;
			pipelineDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
			pipelineDesc.primitive.stripIndexFormat = wgpu::IndexFormat::Undefined;
			pipelineDesc.primitive.frontFace = wgpu::FrontFace::CCW;
			pipelineDesc.primitive.cullMode = wgpu::CullMode::None;
			pipelineDesc.depthStencil = nullptr;  // no depth/stencil on blit
			pipelineDesc.multisample.count = 1;
			pipelineDesc.multisample.mask = 0xFFFFFFFFu;
			pipelineDesc.multisample.alphaToCoverageEnabled = false;

			wgpu::RenderPipeline pipeline = device.CreateRenderPipeline(&pipelineDesc);
			if (!pipeline) {
				IDX_CORE_ERROR_TAG("PostProcessor",
					"CreateRenderPipeline failed for dstFormat={}", static_cast<int>(dstFormat));
				return nullptr;
			}

			impl.BlitPipelines.emplace(dstFormat, pipeline);
			return pipeline;
		}
	}

	void PostProcessor::Blit(const Framebuffer& src,
		wgpu::TextureView dstView,
		wgpu::TextureFormat dstFormat,
		uint32_t dstWidth,
		uint32_t dstHeight)
	{
		if (!IsInitialized()) {
			IDX_CORE_WARN_TAG("PostProcessor", "Blit called before Initialize()");
			return;
		}
		if (!src.IsValid()) {
			IDX_CORE_WARN_TAG("PostProcessor", "Blit src framebuffer is invalid");
			return;
		}
		if (!dstView) {
			IDX_CORE_WARN_TAG("PostProcessor", "Blit dst view is null");
			return;
		}
		if (dstWidth == 0 || dstHeight == 0) return;

		wgpu::Device device = WebGPUBackend::GetDevice();
		if (!device) return;

		// Resolve src framebuffer's color view via the engine's pool.
		WebGPUBackend::FramebufferLookup srcLook =
			WebGPUBackend::LookupFramebufferByFboId(src.GetBackendId());
		if (!srcLook.Valid || !srcLook.ColorView) {
			IDX_CORE_WARN_TAG("PostProcessor",
				"Blit src color view lookup failed (fboId={})", src.GetBackendId());
			return;
		}

		wgpu::RenderPipeline pipeline = GetOrCreateBlitPipeline(*m_Impl, *m_BlitShader, dstFormat);
		if (!pipeline) return;

		// Per-call bind group: source's color view + the shared linear
		// sampler. WebGPU bind groups are cheap to create; the texture
		// view rotates per frame (intermediate FBO is recreated on resize)
		// so we don't try to cache by view pointer.
		wgpu::BindGroupEntry bgEntries[2]{};
		bgEntries[0].binding = 0;
		bgEntries[0].textureView = srcLook.ColorView;
		bgEntries[1].binding = 1;
		bgEntries[1].sampler = m_Impl->LinearSampler;

		wgpu::BindGroupDescriptor bgDesc{};
		bgDesc.label = "postprocess-blit-bg";
		bgDesc.layout = m_Impl->BindGroupLayout;
		bgDesc.entryCount = 2;
		bgDesc.entries = bgEntries;
		wgpu::BindGroup bindGroup = device.CreateBindGroup(&bgDesc);
		if (!bindGroup) {
			IDX_CORE_WARN_TAG("PostProcessor", "CreateBindGroup failed");
			return;
		}

		wgpu::CommandEncoder encoder = WebGPUBackend::GetFrameEncoder();
		if (!encoder) return;

		// Color attachment — LoadOp::Load preserves whatever the caller
		// painted before invoking us (a prior RenderApi::Clear, typically).
		// The fullscreen triangle overwrites the visible region with our
		// sampled source pixels, so the load value only matters for any
		// region outside the dstWidth x dstHeight viewport (none in
		// practice, but Load is the safe default).
		wgpu::RenderPassColorAttachment colorAtt{};
		colorAtt.view       = dstView;
		colorAtt.loadOp     = wgpu::LoadOp::Load;
		colorAtt.storeOp    = wgpu::StoreOp::Store;
		colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

		wgpu::RenderPassDescriptor passDesc{};
		passDesc.label = "postprocess-blit";
		passDesc.colorAttachmentCount = 1;
		passDesc.colorAttachments = &colorAtt;
		passDesc.depthStencilAttachment = nullptr;

		wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
		pass.SetPipeline(pipeline);
		pass.SetBindGroup(0, bindGroup);
		pass.Draw(/*vertexCount=*/3, /*instanceCount=*/1, /*firstVertex=*/0, /*firstInstance=*/0);
		pass.End();
	}

} // namespace Index
