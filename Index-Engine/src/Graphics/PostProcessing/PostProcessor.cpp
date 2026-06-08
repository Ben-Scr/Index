#include "pch.hpp"
#include "Graphics/PostProcessing/PostProcessor.hpp"

#include "Components/Graphics/PostProcessing2DComponent.hpp"
#include "Core/Application.hpp"
#include "Core/Log.hpp"
#include "Core/Time.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/Framebuffer.hpp"

#include <array>
#include <cstring>
#include <unordered_map>


namespace Index {

	struct PostProcessor::Impl {
		// Blit pass (2-binding bgl: texture + sampler).
		wgpu::BindGroupLayout BindGroupLayout;
		wgpu::PipelineLayout  PipelineLayout;
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> BlitPipelines;

		wgpu::BindGroupLayout EffectBindGroupLayout;
		wgpu::PipelineLayout  EffectPipelineLayout;

		wgpu::Buffer          VignetteUniformBuffer;            // 48 bytes
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> VignettePipelines;

		wgpu::Buffer          ChromaticUniformBuffer;           // 16 bytes
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> ChromaticPipelines;

		wgpu::Buffer          GrainUniformBuffer;               // 16 bytes
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> GrainPipelines;

		wgpu::Buffer          LensDistortionUniformBuffer;      // 16 bytes
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> LensDistortionPipelines;

		wgpu::Buffer          ColorGradingUniformBuffer;        // 48 bytes
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> ColorGradingPipelines;

		// Bloom blur H/V use SEPARATE uniform buffers: Dawn's queue.WriteBuffer collapses successive writes
		// to the same buffer within one submission — sharing would make both passes read the vertical direction.
		wgpu::Buffer          BloomThresholdUniformBuffer;      // 16 bytes
		wgpu::Buffer          BloomBlurUniformBufferH;          // 16 bytes
		wgpu::Buffer          BloomBlurUniformBufferV;          // 16 bytes
		wgpu::Buffer          BloomCompositeUniformBuffer;      // 16 bytes
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> BloomThresholdPipelines;
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> BloomBlurPipelines;
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> BloomCompositePipelines;
		wgpu::BindGroupLayout BloomCompositeBgl;
		wgpu::PipelineLayout  BloomCompositePipelineLayout;
		Framebuffer           BloomTempA;
		Framebuffer           BloomTempB;

		wgpu::Buffer          PixelatedUniformBuffer;           // 16 bytes
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> PixelatedPipelines;

		wgpu::Buffer          GaussianBlurUniformBufferH;       // 16 bytes
		wgpu::Buffer          GaussianBlurUniformBufferV;       // 16 bytes
		std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> GaussianBlurPipelines;
		Framebuffer           GaussianBlurTemp;                 // full-res RGBA16F

		Framebuffer           PingPongA;
		Framebuffer           PingPongB;

		// Shared.
		wgpu::Sampler         LinearSampler;
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

		m_BlitShader = std::make_unique<Shader>("postblit", "postblit");
		if (!m_BlitShader->IsValid()) {
			IDX_CORE_ERROR_TAG("PostProcessor",
				"Failed to compile post-blit shader (built-in lookup miss)");
			m_BlitShader.reset();
			return false;
		}

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

		wgpu::BindGroupLayoutEntry effectBglEntries[3]{};
		effectBglEntries[0].binding = 0;
		effectBglEntries[0].visibility = wgpu::ShaderStage::Fragment;
		effectBglEntries[0].texture.sampleType = wgpu::TextureSampleType::Float;
		effectBglEntries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
		effectBglEntries[0].texture.multisampled = false;

		effectBglEntries[1].binding = 1;
		effectBglEntries[1].visibility = wgpu::ShaderStage::Fragment;
		effectBglEntries[1].sampler.type = wgpu::SamplerBindingType::Filtering;

		effectBglEntries[2].binding = 2;
		effectBglEntries[2].visibility = wgpu::ShaderStage::Fragment;
		effectBglEntries[2].buffer.type = wgpu::BufferBindingType::Uniform;
		effectBglEntries[2].buffer.hasDynamicOffset = false;
		effectBglEntries[2].buffer.minBindingSize = 0;

		wgpu::BindGroupLayoutDescriptor effectBglDesc{};
		effectBglDesc.label = "postprocess-effect-bgl";
		effectBglDesc.entryCount = 3;
		effectBglDesc.entries = effectBglEntries;
		m_Impl->EffectBindGroupLayout = device.CreateBindGroupLayout(&effectBglDesc);
		if (!m_Impl->EffectBindGroupLayout) {
			IDX_CORE_ERROR_TAG("PostProcessor", "CreateBindGroupLayout (effect) failed");
			return false;
		}

		wgpu::PipelineLayoutDescriptor effectPlDesc{};
		effectPlDesc.label = "postprocess-effect-pipeline-layout";
		effectPlDesc.bindGroupLayoutCount = 1;
		effectPlDesc.bindGroupLayouts = &m_Impl->EffectBindGroupLayout;
		m_Impl->EffectPipelineLayout = device.CreatePipelineLayout(&effectPlDesc);
		if (!m_Impl->EffectPipelineLayout) {
			IDX_CORE_ERROR_TAG("PostProcessor", "CreatePipelineLayout (effect) failed");
			return false;
		}

		auto makeUniformBuffer = [&](const char* label, uint64_t size) -> wgpu::Buffer {
			wgpu::BufferDescriptor d{};
			d.label = label;
			d.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
			d.size = size;
			d.mappedAtCreation = false;
			return device.CreateBuffer(&d);
		};

		m_Impl->VignetteUniformBuffer        = makeUniformBuffer("postprocess-vignette-uniforms", 48);
		m_Impl->ChromaticUniformBuffer       = makeUniformBuffer("postprocess-chromatic-uniforms", 16);
		m_Impl->GrainUniformBuffer           = makeUniformBuffer("postprocess-grain-uniforms", 16);
		m_Impl->LensDistortionUniformBuffer  = makeUniformBuffer("postprocess-lens-uniforms", 16);
		m_Impl->ColorGradingUniformBuffer    = makeUniformBuffer("postprocess-colorgrading-uniforms", 48);
		m_Impl->BloomThresholdUniformBuffer  = makeUniformBuffer("postprocess-bloom-threshold-uniforms", 16);
		m_Impl->BloomBlurUniformBufferH      = makeUniformBuffer("postprocess-bloom-blur-h-uniforms", 16);
		m_Impl->BloomBlurUniformBufferV      = makeUniformBuffer("postprocess-bloom-blur-v-uniforms", 16);
		m_Impl->BloomCompositeUniformBuffer  = makeUniformBuffer("postprocess-bloom-composite-uniforms", 16);
		m_Impl->PixelatedUniformBuffer       = makeUniformBuffer("postprocess-pixelated-uniforms", 16);
		m_Impl->GaussianBlurUniformBufferH   = makeUniformBuffer("postprocess-gaussian-blur-h-uniforms", 16);
		m_Impl->GaussianBlurUniformBufferV   = makeUniformBuffer("postprocess-gaussian-blur-v-uniforms", 16);

		if (!m_Impl->VignetteUniformBuffer
			|| !m_Impl->ChromaticUniformBuffer
			|| !m_Impl->GrainUniformBuffer
			|| !m_Impl->LensDistortionUniformBuffer
			|| !m_Impl->ColorGradingUniformBuffer
			|| !m_Impl->BloomThresholdUniformBuffer
			|| !m_Impl->BloomBlurUniformBufferH
			|| !m_Impl->BloomBlurUniformBufferV
			|| !m_Impl->BloomCompositeUniformBuffer
			|| !m_Impl->PixelatedUniformBuffer
			|| !m_Impl->GaussianBlurUniformBufferH
			|| !m_Impl->GaussianBlurUniformBufferV)
		{
			IDX_CORE_ERROR_TAG("PostProcessor", "CreateBuffer (per-effect uniform) failed");
			return false;
		}

		// Bloom composite uses 4 bindings (scene + sampler + bloom + uniform) unlike all other effects.
		wgpu::BindGroupLayoutEntry compositeBglEntries[4]{};
		compositeBglEntries[0].binding = 0;
		compositeBglEntries[0].visibility = wgpu::ShaderStage::Fragment;
		compositeBglEntries[0].texture.sampleType = wgpu::TextureSampleType::Float;
		compositeBglEntries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;

		compositeBglEntries[1].binding = 1;
		compositeBglEntries[1].visibility = wgpu::ShaderStage::Fragment;
		compositeBglEntries[1].sampler.type = wgpu::SamplerBindingType::Filtering;

		compositeBglEntries[2].binding = 2;
		compositeBglEntries[2].visibility = wgpu::ShaderStage::Fragment;
		compositeBglEntries[2].texture.sampleType = wgpu::TextureSampleType::Float;
		compositeBglEntries[2].texture.viewDimension = wgpu::TextureViewDimension::e2D;

		compositeBglEntries[3].binding = 3;
		compositeBglEntries[3].visibility = wgpu::ShaderStage::Fragment;
		compositeBglEntries[3].buffer.type = wgpu::BufferBindingType::Uniform;
		compositeBglEntries[3].buffer.hasDynamicOffset = false;
		compositeBglEntries[3].buffer.minBindingSize = 0;

		wgpu::BindGroupLayoutDescriptor compositeBglDesc{};
		compositeBglDesc.label = "postprocess-bloom-composite-bgl";
		compositeBglDesc.entryCount = 4;
		compositeBglDesc.entries = compositeBglEntries;
		m_Impl->BloomCompositeBgl = device.CreateBindGroupLayout(&compositeBglDesc);
		if (!m_Impl->BloomCompositeBgl) {
			IDX_CORE_ERROR_TAG("PostProcessor", "CreateBindGroupLayout (bloom composite) failed");
			return false;
		}

		wgpu::PipelineLayoutDescriptor compositePlDesc{};
		compositePlDesc.label = "postprocess-bloom-composite-pipeline-layout";
		compositePlDesc.bindGroupLayoutCount = 1;
		compositePlDesc.bindGroupLayouts = &m_Impl->BloomCompositeBgl;
		m_Impl->BloomCompositePipelineLayout = device.CreatePipelineLayout(&compositePlDesc);
		if (!m_Impl->BloomCompositePipelineLayout) {
			IDX_CORE_ERROR_TAG("PostProcessor", "CreatePipelineLayout (bloom composite) failed");
			return false;
		}

		auto loadShader = [](const char* name) -> std::unique_ptr<Shader> {
			auto s = std::make_unique<Shader>(name, name);
			if (!s->IsValid()) {
				IDX_CORE_ERROR_TAG("PostProcessor",
					"Failed to compile post-effect shader '{}' (built-in lookup miss)",
					name);
				return nullptr;
			}
			return s;
		};

		m_VignetteShader        = loadShader("postvignette");
		m_ChromaticShader       = loadShader("postchromatic");
		m_GrainShader           = loadShader("postgrain");
		m_LensDistortionShader  = loadShader("postlensdistortion");
		m_ColorGradingShader    = loadShader("postcolorgrading");
		m_BloomThresholdShader  = loadShader("postbloomthreshold");
		m_BloomBlurShader       = loadShader("postbloomblur");
		m_BloomCompositeShader  = loadShader("postbloomcomposite");
		m_PixelatedShader       = loadShader("postpixelated");

		if (!m_VignetteShader || !m_ChromaticShader || !m_GrainShader
			|| !m_LensDistortionShader || !m_ColorGradingShader
			|| !m_BloomThresholdShader || !m_BloomBlurShader || !m_BloomCompositeShader
			|| !m_PixelatedShader)
		{
			m_VignetteShader.reset();
			m_ChromaticShader.reset();
			m_GrainShader.reset();
			m_LensDistortionShader.reset();
			m_ColorGradingShader.reset();
			m_BloomThresholdShader.reset();
			m_BloomBlurShader.reset();
			m_BloomCompositeShader.reset();
			m_PixelatedShader.reset();
			return false;
		}

		m_Impl->Initialized = true;
		return true;
	}

	void PostProcessor::Shutdown() {
		if (!m_Impl) return;

		m_Impl->VignettePipelines.clear();
		m_Impl->ChromaticPipelines.clear();
		m_Impl->GrainPipelines.clear();
		m_Impl->LensDistortionPipelines.clear();
		m_Impl->ColorGradingPipelines.clear();
		m_Impl->BloomThresholdPipelines.clear();
		m_Impl->BloomBlurPipelines.clear();
		m_Impl->BloomCompositePipelines.clear();
		m_Impl->PixelatedPipelines.clear();
		m_Impl->GaussianBlurPipelines.clear();

		m_Impl->VignetteUniformBuffer = nullptr;
		m_Impl->ChromaticUniformBuffer = nullptr;
		m_Impl->GrainUniformBuffer = nullptr;
		m_Impl->LensDistortionUniformBuffer = nullptr;
		m_Impl->ColorGradingUniformBuffer = nullptr;
		m_Impl->BloomThresholdUniformBuffer = nullptr;
		m_Impl->BloomBlurUniformBufferH = nullptr;
		m_Impl->BloomBlurUniformBufferV = nullptr;
		m_Impl->BloomCompositeUniformBuffer = nullptr;
		m_Impl->PixelatedUniformBuffer = nullptr;
		m_Impl->GaussianBlurUniformBufferH = nullptr;
		m_Impl->GaussianBlurUniformBufferV = nullptr;

		m_Impl->BloomCompositePipelineLayout = nullptr;
		m_Impl->BloomCompositeBgl = nullptr;
		m_Impl->BloomTempA.Destroy();
		m_Impl->BloomTempB.Destroy();
		m_Impl->GaussianBlurTemp.Destroy();

		m_Impl->PingPongA.Destroy();
		m_Impl->PingPongB.Destroy();
		m_Impl->EffectPipelineLayout = nullptr;
		m_Impl->EffectBindGroupLayout = nullptr;
		m_Impl->BlitPipelines.clear();
		m_Impl->LinearSampler = nullptr;
		m_Impl->PipelineLayout = nullptr;
		m_Impl->BindGroupLayout = nullptr;
		m_Impl->Initialized = false;

		m_PixelatedShader.reset();
		m_BloomCompositeShader.reset();
		m_BloomBlurShader.reset();
		m_BloomThresholdShader.reset();
		m_ColorGradingShader.reset();
		m_LensDistortionShader.reset();
		m_GrainShader.reset();
		m_ChromaticShader.reset();
		m_VignetteShader.reset();
		m_BlitShader.reset();
	}

	namespace {
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

			wgpu::VertexState vertexState{};
			vertexState.module = look.Module;
			vertexState.entryPoint = "vs_main";
			vertexState.bufferCount = 0;
			vertexState.buffers = nullptr;

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

		wgpu::RenderPipeline BuildEffectPipeline(
			PostProcessor::Impl& impl,
			const Shader& shader,
			wgpu::TextureFormat dstFormat,
			const char* label)
		{
			wgpu::Device device = WebGPUBackend::GetDevice();
			if (!device) return nullptr;

			WebGPUBackend::ShaderLookup look = WebGPUBackend::LookupShader(shader.GetHandle());
			if (!look.Valid) {
				IDX_CORE_ERROR_TAG("PostProcessor",
					"{} shader module not resolvable (handle={})", label, shader.GetHandle());
				return nullptr;
			}

			wgpu::VertexState vertexState{};
			vertexState.module = look.Module;
			vertexState.entryPoint = "vs_main";
			vertexState.bufferCount = 0;
			vertexState.buffers = nullptr;

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
			pipelineDesc.label = label;
			pipelineDesc.layout = impl.EffectPipelineLayout;
			pipelineDesc.vertex = vertexState;
			pipelineDesc.fragment = &fragmentState;
			pipelineDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
			pipelineDesc.primitive.stripIndexFormat = wgpu::IndexFormat::Undefined;
			pipelineDesc.primitive.frontFace = wgpu::FrontFace::CCW;
			pipelineDesc.primitive.cullMode = wgpu::CullMode::None;
			pipelineDesc.depthStencil = nullptr;
			pipelineDesc.multisample.count = 1;
			pipelineDesc.multisample.mask = 0xFFFFFFFFu;
			pipelineDesc.multisample.alphaToCoverageEnabled = false;

			wgpu::RenderPipeline pipeline = device.CreateRenderPipeline(&pipelineDesc);
			if (!pipeline) {
				IDX_CORE_ERROR_TAG("PostProcessor",
					"{} CreateRenderPipeline failed for dstFormat={}",
					label, static_cast<int>(dstFormat));
			}
			return pipeline;
		}

		wgpu::RenderPipeline GetOrCreateEffectPipeline(
			PostProcessor::Impl& impl,
			const Shader& shader,
			std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline>& cache,
			wgpu::TextureFormat dstFormat,
			const char* label)
		{
			auto it = cache.find(dstFormat);
			if (it != cache.end()) return it->second;
			wgpu::RenderPipeline pipeline = BuildEffectPipeline(impl, shader, dstFormat, label);
			if (pipeline) cache.emplace(dstFormat, pipeline);
			return pipeline;
		}

		void RunEffectPass(
			PostProcessor::Impl& impl,
			wgpu::RenderPipeline pipeline,
			wgpu::TextureView srcView,
			wgpu::TextureView dstView,
			wgpu::Buffer uniformBuffer,
			const void* uniformData,
			size_t uniformSize,
			const char* passLabel)
		{
			wgpu::Device device = WebGPUBackend::GetDevice();
			wgpu::Queue  queue  = WebGPUBackend::GetQueue();
			if (!device || !queue || !pipeline || !srcView || !dstView) return;

			queue.WriteBuffer(uniformBuffer, 0, uniformData, uniformSize);

			wgpu::BindGroupEntry entries[3]{};
			entries[0].binding = 0;
			entries[0].textureView = srcView;
			entries[1].binding = 1;
			entries[1].sampler = impl.LinearSampler;
			entries[2].binding = 2;
			entries[2].buffer  = uniformBuffer;
			entries[2].offset  = 0;
			entries[2].size    = uniformSize;

			wgpu::BindGroupDescriptor bgDesc{};
			bgDesc.label = passLabel;
			bgDesc.layout = impl.EffectBindGroupLayout;
			bgDesc.entryCount = 3;
			bgDesc.entries = entries;
			wgpu::BindGroup bg = device.CreateBindGroup(&bgDesc);
			if (!bg) {
				IDX_CORE_WARN_TAG("PostProcessor", "{} CreateBindGroup failed", passLabel);
				return;
			}

			wgpu::CommandEncoder encoder = WebGPUBackend::GetFrameEncoder();
			if (!encoder) return;

			wgpu::RenderPassColorAttachment colorAtt{};
			colorAtt.view       = dstView;
			colorAtt.loadOp     = wgpu::LoadOp::Load;
			colorAtt.storeOp    = wgpu::StoreOp::Store;
			colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

			wgpu::RenderPassDescriptor passDesc{};
			passDesc.label = passLabel;
			passDesc.colorAttachmentCount = 1;
			passDesc.colorAttachments = &colorAtt;
			passDesc.depthStencilAttachment = nullptr;

			wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
			pass.SetPipeline(pipeline);
			pass.SetBindGroup(0, bg);
			pass.Draw(/*vertexCount=*/3, /*instanceCount=*/1, /*firstVertex=*/0, /*firstInstance=*/0);
			pass.End();
		}

		wgpu::TextureView ViewOf(const Framebuffer& fb) {
			WebGPUBackend::FramebufferLookup look =
				WebGPUBackend::LookupFramebufferByFboId(fb.GetBackendId());
			return look.Valid ? look.ColorView : nullptr;
		}

		// Clear a target to transparent via an empty load-clear render pass. Effect
		// passes write with LoadOp::Load + alpha blend, so a ping-pong reused across
		// frames must be zeroed first — otherwise a transparent (alpha-0) scene
		// background blends against the previous frame's contents and smears.
		void ClearViewTransparent(wgpu::TextureView view) {
			if (!view) return;
			wgpu::CommandEncoder encoder = WebGPUBackend::GetFrameEncoder();
			if (!encoder) return;

			wgpu::RenderPassColorAttachment colorAtt{};
			colorAtt.view       = view;
			colorAtt.loadOp     = wgpu::LoadOp::Clear;
			colorAtt.storeOp    = wgpu::StoreOp::Store;
			colorAtt.clearValue = wgpu::Color{ 0.0, 0.0, 0.0, 0.0 };
			colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

			wgpu::RenderPassDescriptor passDesc{};
			passDesc.label                = "postprocess-pingpong-clear";
			passDesc.colorAttachmentCount = 1;
			passDesc.colorAttachments     = &colorAtt;

			wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
			pass.End();
		}

		bool EnsurePingPongFbos(PostProcessor::Impl& impl, uint32_t w, uint32_t h) {
			const int iw = static_cast<int>(w);
			const int ih = static_cast<int>(h);
			if (!impl.PingPongA.Recreate(iw, ih, TextureFormat::RGBA16F)) return false;
			if (!impl.PingPongB.Recreate(iw, ih, TextureFormat::RGBA16F)) return false;
			return true;
		}

		void RunVignettePass(PostProcessor::Impl& impl, const Shader& shader,
			wgpu::TextureView srcView, wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			uint32_t dstWidth, uint32_t dstHeight,
			const PostProcessing2DComponent::VignetteSettings& v)
		{
			wgpu::RenderPipeline pipeline = GetOrCreateEffectPipeline(
				impl, shader, impl.VignettePipelines, dstFormat,
				"postprocess-vignette-pipeline");
			if (!pipeline) return;

			float u[12]{};
			u[0]  = v.Color.r; u[1] = v.Color.g; u[2] = v.Color.b; u[3] = v.Color.a;
			u[4]  = v.Center.x; u[5] = v.Center.y; u[6] = v.Intensity; u[7] = v.Smoothness;
			u[8]  = (dstHeight > 0)
				? static_cast<float>(dstWidth) / static_cast<float>(dstHeight)
				: 1.0f;
			u[9]  = v.Roundness;
			RunEffectPass(impl, pipeline, srcView, dstView,
				impl.VignetteUniformBuffer, u, sizeof(u), "postprocess-vignette");
		}

		void RunChromaticPass(PostProcessor::Impl& impl, const Shader& shader,
			wgpu::TextureView srcView, wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			const PostProcessing2DComponent::ChromaticAberrationSettings& c)
		{
			wgpu::RenderPipeline pipeline = GetOrCreateEffectPipeline(
				impl, shader, impl.ChromaticPipelines, dstFormat,
				"postprocess-chromatic-pipeline");
			if (!pipeline) return;

			float u[4]{};
			u[0] = c.Intensity;
			RunEffectPass(impl, pipeline, srcView, dstView,
				impl.ChromaticUniformBuffer, u, sizeof(u), "postprocess-chromatic");
		}

		void RunGrainPass(PostProcessor::Impl& impl, const Shader& shader,
			wgpu::TextureView srcView, wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			const PostProcessing2DComponent::GrainSettings& g, float timeSeconds)
		{
			wgpu::RenderPipeline pipeline = GetOrCreateEffectPipeline(
				impl, shader, impl.GrainPipelines, dstFormat,
				"postprocess-grain-pipeline");
			if (!pipeline) return;

			float u[4]{};
			u[0] = g.Intensity;
			u[1] = g.Size;
			u[2] = g.Colored ? 1.0f : 0.0f;
			u[3] = timeSeconds;
			RunEffectPass(impl, pipeline, srcView, dstView,
				impl.GrainUniformBuffer, u, sizeof(u), "postprocess-grain");
		}

		void RunLensDistortionPass(PostProcessor::Impl& impl, const Shader& shader,
			wgpu::TextureView srcView, wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			const PostProcessing2DComponent::LensDistortionSettings& l)
		{
			wgpu::RenderPipeline pipeline = GetOrCreateEffectPipeline(
				impl, shader, impl.LensDistortionPipelines, dstFormat,
				"postprocess-lensdistortion-pipeline");
			if (!pipeline) return;

			float u[4]{};
			u[0] = l.Intensity;
			u[1] = l.Scale;
			u[2] = l.Center.x;
			u[3] = l.Center.y;
			RunEffectPass(impl, pipeline, srcView, dstView,
				impl.LensDistortionUniformBuffer, u, sizeof(u), "postprocess-lensdistortion");
		}

		void RunColorGradingPass(PostProcessor::Impl& impl, const Shader& shader,
			wgpu::TextureView srcView, wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			const PostProcessing2DComponent::ColorGradingSettings& cg)
		{
			wgpu::RenderPipeline pipeline = GetOrCreateEffectPipeline(
				impl, shader, impl.ColorGradingPipelines, dstFormat,
				"postprocess-colorgrading-pipeline");
			if (!pipeline) return;

			float u[12]{};
			// colorFilter
			u[0] = cg.ColorFilter.r; u[1] = cg.ColorFilter.g;
			u[2] = cg.ColorFilter.b; u[3] = cg.ColorFilter.a;
			// exposure / contrast / saturation
			u[4] = cg.Exposure; u[5] = cg.Contrast; u[6] = cg.Saturation; u[7] = 0.0f;
			// temperature / tint
			u[8] = cg.Temperature; u[9] = cg.Tint; u[10] = 0.0f; u[11] = 0.0f;
			RunEffectPass(impl, pipeline, srcView, dstView,
				impl.ColorGradingUniformBuffer, u, sizeof(u), "postprocess-colorgrading");
		}

		wgpu::RenderPipeline BuildBloomCompositePipeline(
			PostProcessor::Impl& impl,
			const Shader& shader,
			wgpu::TextureFormat dstFormat)
		{
			wgpu::Device device = WebGPUBackend::GetDevice();
			if (!device) return nullptr;

			WebGPUBackend::ShaderLookup look = WebGPUBackend::LookupShader(shader.GetHandle());
			if (!look.Valid) {
				IDX_CORE_ERROR_TAG("PostProcessor",
					"Bloom composite shader module not resolvable");
				return nullptr;
			}

			wgpu::VertexState vertexState{};
			vertexState.module = look.Module;
			vertexState.entryPoint = "vs_main";
			vertexState.bufferCount = 0;

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

			wgpu::RenderPipelineDescriptor desc{};
			desc.label = "postprocess-bloom-composite-pipeline";
			desc.layout = impl.BloomCompositePipelineLayout;
			desc.vertex = vertexState;
			desc.fragment = &fragmentState;
			desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
			desc.primitive.frontFace = wgpu::FrontFace::CCW;
			desc.primitive.cullMode = wgpu::CullMode::None;
			desc.multisample.count = 1;
			desc.multisample.mask = 0xFFFFFFFFu;

			wgpu::RenderPipeline pipeline = device.CreateRenderPipeline(&desc);
			if (!pipeline) {
				IDX_CORE_ERROR_TAG("PostProcessor",
					"CreateRenderPipeline (bloom composite) failed for dstFormat={}",
					static_cast<int>(dstFormat));
			}
			return pipeline;
		}

		wgpu::RenderPipeline GetOrCreateBloomCompositePipeline(
			PostProcessor::Impl& impl,
			const Shader& shader,
			wgpu::TextureFormat dstFormat)
		{
			auto it = impl.BloomCompositePipelines.find(dstFormat);
			if (it != impl.BloomCompositePipelines.end()) return it->second;
			wgpu::RenderPipeline p = BuildBloomCompositePipeline(impl, shader, dstFormat);
			if (p) impl.BloomCompositePipelines.emplace(dstFormat, p);
			return p;
		}

		void RunBloomCompositePass(
			PostProcessor::Impl& impl,
			wgpu::RenderPipeline pipeline,
			wgpu::TextureView sceneView,
			wgpu::TextureView bloomView,
			wgpu::TextureView dstView,
			wgpu::Buffer uniformBuffer,
			const void* uniformData,
			size_t uniformSize)
		{
			wgpu::Device device = WebGPUBackend::GetDevice();
			wgpu::Queue  queue  = WebGPUBackend::GetQueue();
			if (!device || !queue || !pipeline
				|| !sceneView || !bloomView || !dstView) return;

			queue.WriteBuffer(uniformBuffer, 0, uniformData, uniformSize);

			wgpu::BindGroupEntry entries[4]{};
			entries[0].binding = 0;
			entries[0].textureView = sceneView;
			entries[1].binding = 1;
			entries[1].sampler = impl.LinearSampler;
			entries[2].binding = 2;
			entries[2].textureView = bloomView;
			entries[3].binding = 3;
			entries[3].buffer = uniformBuffer;
			entries[3].offset = 0;
			entries[3].size = uniformSize;

			wgpu::BindGroupDescriptor bgDesc{};
			bgDesc.label = "postprocess-bloom-composite-bg";
			bgDesc.layout = impl.BloomCompositeBgl;
			bgDesc.entryCount = 4;
			bgDesc.entries = entries;
			wgpu::BindGroup bg = device.CreateBindGroup(&bgDesc);
			if (!bg) {
				IDX_CORE_WARN_TAG("PostProcessor",
					"Bloom composite CreateBindGroup failed");
				return;
			}

			wgpu::CommandEncoder encoder = WebGPUBackend::GetFrameEncoder();
			if (!encoder) return;

			wgpu::RenderPassColorAttachment colorAtt{};
			colorAtt.view = dstView;
			colorAtt.loadOp = wgpu::LoadOp::Load;
			colorAtt.storeOp = wgpu::StoreOp::Store;
			colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

			wgpu::RenderPassDescriptor passDesc{};
			passDesc.label = "postprocess-bloom-composite";
			passDesc.colorAttachmentCount = 1;
			passDesc.colorAttachments = &colorAtt;

			wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
			pass.SetPipeline(pipeline);
			pass.SetBindGroup(0, bg);
			pass.Draw(3, 1, 0, 0);
			pass.End();
		}

		void RunBloomPass(
			PostProcessor::Impl& impl,
			const Shader& thresholdShader,
			const Shader& blurShader,
			const Shader& compositeShader,
			wgpu::TextureView srcView,
			wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			uint32_t dstWidth, uint32_t dstHeight,
			const PostProcessing2DComponent::BloomSettings& b)
		{
			const int fullW = std::max(1, static_cast<int>(dstWidth));
			const int fullH = std::max(1, static_cast<int>(dstHeight));
			if (!impl.BloomTempA.Recreate(fullW, fullH, TextureFormat::RGBA16F)) return;
			if (!impl.BloomTempB.Recreate(fullW, fullH, TextureFormat::RGBA16F)) return;

			WebGPUBackend::FramebufferLookup tempALook =
				WebGPUBackend::LookupFramebufferByFboId(impl.BloomTempA.GetBackendId());
			WebGPUBackend::FramebufferLookup tempBLook =
				WebGPUBackend::LookupFramebufferByFboId(impl.BloomTempB.GetBackendId());
			if (!tempALook.Valid || !tempBLook.Valid) return;

			const wgpu::TextureFormat tempFormat = wgpu::TextureFormat::RGBA16Float;

			const int tapsClamped = std::max(3, std::min(500, b.Taps));
			const float halfKernel = static_cast<float>(std::max(1, (tapsClamped - 1) / 2));

			// ── Pass 1: Threshold extract (srcView -> BloomTempA) ──
			{
				wgpu::RenderPipeline thresholdPipeline = GetOrCreateEffectPipeline(
					impl, thresholdShader, impl.BloomThresholdPipelines, tempFormat,
					"postprocess-bloom-threshold-pipeline");
				if (!thresholdPipeline) return;

				float u[4]{};
				u[0] = b.Threshold;
				RunEffectPass(impl, thresholdPipeline, srcView, tempALook.ColorView,
					impl.BloomThresholdUniformBuffer, u, sizeof(u),
					"postprocess-bloom-threshold");
			}

			// ── Pass 2: Horizontal blur (BloomTempA -> BloomTempB) ──
			wgpu::RenderPipeline blurPipeline = GetOrCreateEffectPipeline(
				impl, blurShader, impl.BloomBlurPipelines, tempFormat,
				"postprocess-bloom-blur-pipeline");
			if (!blurPipeline) return;

			const float scatterMul = 1.0f + 10.0f * b.Scatter;   // [1.0 .. 11.0]
			const float texelW = scatterMul / static_cast<float>(fullW);
			const float texelH = scatterMul / static_cast<float>(fullH);

			{
				float u[4]{};
				u[0] = 1.0f; u[1] = 0.0f; u[2] = texelW; u[3] = halfKernel;
				RunEffectPass(impl, blurPipeline,
					tempALook.ColorView, tempBLook.ColorView,
					impl.BloomBlurUniformBufferH, u, sizeof(u),
					"postprocess-bloom-blur-h");
			}

			// ── Pass 3: Vertical blur (BloomTempB -> BloomTempA) ──
			{
				float u[4]{};
				u[0] = 0.0f; u[1] = 1.0f; u[2] = texelH; u[3] = halfKernel;
				RunEffectPass(impl, blurPipeline,
					tempBLook.ColorView, tempALook.ColorView,
					impl.BloomBlurUniformBufferV, u, sizeof(u),
					"postprocess-bloom-blur-v");
			}

			// ── Pass 4: Composite scene + bloom -> dstView ──
			wgpu::RenderPipeline compositePipeline = GetOrCreateBloomCompositePipeline(
				impl, compositeShader, dstFormat);
			if (!compositePipeline) return;

			float u[4]{};
			u[0] = b.Tint.r * b.Tint.a;
			u[1] = b.Tint.g * b.Tint.a;
			u[2] = b.Tint.b * b.Tint.a;
			u[3] = b.Intensity;
			RunBloomCompositePass(impl, compositePipeline,
				srcView, tempALook.ColorView, dstView,
				impl.BloomCompositeUniformBuffer, u, sizeof(u));
		}

		void RunGaussianBlurPass(
			PostProcessor::Impl& impl,
			const Shader& blurShader,
			wgpu::TextureView srcView,
			wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			uint32_t dstWidth, uint32_t dstHeight,
			const PostProcessing2DComponent::GaussianBlurSettings& g)
		{
			const int fullW = std::max(1, static_cast<int>(dstWidth));
			const int fullH = std::max(1, static_cast<int>(dstHeight));
			if (!impl.GaussianBlurTemp.Recreate(fullW, fullH, TextureFormat::RGBA16F)) return;

			WebGPUBackend::FramebufferLookup tempLook =
				WebGPUBackend::LookupFramebufferByFboId(impl.GaussianBlurTemp.GetBackendId());
			if (!tempLook.Valid || !tempLook.ColorView) return;

			wgpu::RenderPipeline pipelineToTemp = GetOrCreateEffectPipeline(
				impl, blurShader, impl.GaussianBlurPipelines,
				wgpu::TextureFormat::RGBA16Float,
				"postprocess-gaussianblur-pipeline-temp");
			wgpu::RenderPipeline pipelineToDst = GetOrCreateEffectPipeline(
				impl, blurShader, impl.GaussianBlurPipelines, dstFormat,
				"postprocess-gaussianblur-pipeline-dst");
			if (!pipelineToTemp || !pipelineToDst) return;

			const int tapsClamped = std::max(3, std::min(500, g.Taps));
			const float halfKernel = static_cast<float>(std::max(1, (tapsClamped - 1) / 2));

			const float radiusMul = 0.5f + 6.0f * g.Radius;
			const float texelW = radiusMul / static_cast<float>(fullW);
			const float texelH = radiusMul / static_cast<float>(fullH);

			// ── Pass 1: Horizontal blur (srcView -> GaussianBlurTemp) ──
			{
				float u[4]{};
				u[0] = 1.0f; u[1] = 0.0f; u[2] = texelW; u[3] = halfKernel;
				RunEffectPass(impl, pipelineToTemp, srcView, tempLook.ColorView,
					impl.GaussianBlurUniformBufferH, u, sizeof(u),
					"postprocess-gaussianblur-h");
			}

			// ── Pass 2: Vertical blur (GaussianBlurTemp -> dstView) ──
			{
				float u[4]{};
				u[0] = 0.0f; u[1] = 1.0f; u[2] = texelH; u[3] = halfKernel;
				RunEffectPass(impl, pipelineToDst, tempLook.ColorView, dstView,
					impl.GaussianBlurUniformBufferV, u, sizeof(u),
					"postprocess-gaussianblur-v");
			}
		}

		void RunPixelatedPass(PostProcessor::Impl& impl, const Shader& shader,
			wgpu::TextureView srcView, wgpu::TextureView dstView,
			wgpu::TextureFormat dstFormat,
			uint32_t dstWidth, uint32_t dstHeight,
			const PostProcessing2DComponent::PixelatedSettings& p)
		{
			wgpu::RenderPipeline pipeline = GetOrCreateEffectPipeline(
				impl, shader, impl.PixelatedPipelines, dstFormat,
				"postprocess-pixelated-pipeline");
			if (!pipeline) return;

			float u[4]{};
			u[0] = p.BlockSize;
			u[1] = static_cast<float>(dstWidth);
			u[2] = static_cast<float>(dstHeight);
			// 0 disables palette quantisation in the shader; >= 2 sets the
			// per-channel level count.
			u[3] = p.QuantizeColor
				? static_cast<float>(p.PaletteSteps < 2 ? 2 : p.PaletteSteps)
				: 0.0f;
			RunEffectPass(impl, pipeline, srcView, dstView,
				impl.PixelatedUniformBuffer, u, sizeof(u), "postprocess-pixelated");
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

		WebGPUBackend::FramebufferLookup srcLook =
			WebGPUBackend::LookupFramebufferByFboId(src.GetBackendId());
		if (!srcLook.Valid || !srcLook.ColorView) {
			IDX_CORE_WARN_TAG("PostProcessor",
				"Blit src color view lookup failed (fboId={})", src.GetBackendId());
			return;
		}

		wgpu::RenderPipeline pipeline = GetOrCreateBlitPipeline(*m_Impl, *m_BlitShader, dstFormat);
		if (!pipeline) return;

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
		// MUST apply viewport before draw: aspect-lock caller sets a sub-rect so the blit
		// respects the letterbox region painted by the swap-chain clear.
		WebGPUBackend::ApplyCachedViewportToPass(pass);
		pass.SetPipeline(pipeline);
		pass.SetBindGroup(0, bindGroup);
		pass.Draw(/*vertexCount=*/3, /*instanceCount=*/1, /*firstVertex=*/0, /*firstInstance=*/0);
		pass.End();
	}

	void PostProcessor::Run(const Framebuffer& src,
		wgpu::TextureView dstView,
		wgpu::TextureFormat dstFormat,
		uint32_t dstWidth,
		uint32_t dstHeight,
		const PostProcessing2DComponent* settings)
	{
		if (!IsInitialized()) {
			IDX_CORE_WARN_TAG("PostProcessor", "Run called before Initialize()");
			return;
		}
		if (!src.IsValid() || !dstView || dstWidth == 0 || dstHeight == 0) {
			return;
		}

		// No component on the camera → passthrough.
		if (!settings) {
			Blit(src, dstView, dstFormat, dstWidth, dstHeight);
			return;
		}

		// Effect order: Bloom (needs HDR signal) → ColorGrading → GaussianBlur → Lens → CA → Vignette → Grain → Pixelated.
		enum class EffectKind : uint8_t {
			Bloom, ColorGrading, GaussianBlur, LensDistortion, Chromatic, Vignette, Grain, Pixelated
		};
		std::array<EffectKind, 8> enabled{};
		size_t enabledCount = 0;
		if (settings->Bloom.Enabled)               enabled[enabledCount++] = EffectKind::Bloom;
		if (settings->ColorGrading.Enabled)        enabled[enabledCount++] = EffectKind::ColorGrading;
		if (settings->GaussianBlur.Enabled)        enabled[enabledCount++] = EffectKind::GaussianBlur;
		if (settings->LensDistortion.Enabled)      enabled[enabledCount++] = EffectKind::LensDistortion;
		if (settings->ChromaticAberration.Enabled) enabled[enabledCount++] = EffectKind::Chromatic;
		if (settings->Vignette.Enabled)            enabled[enabledCount++] = EffectKind::Vignette;
		if (settings->Grain.Enabled)               enabled[enabledCount++] = EffectKind::Grain;
		if (settings->Pixelated.Enabled)           enabled[enabledCount++] = EffectKind::Pixelated;

		if (enabledCount == 0) {
			Blit(src, dstView, dstFormat, dstWidth, dstHeight);
			return;
		}

		if (enabledCount > 1) {
			if (!EnsurePingPongFbos(*m_Impl, dstWidth, dstHeight)) {
				IDX_CORE_WARN_TAG("PostProcessor",
					"EnsurePingPongFbos failed at {}x{} — falling back to Blit",
					dstWidth, dstHeight);
				Blit(src, dstView, dstFormat, dstWidth, dstHeight);
				return;
			}
			// Ping-pong FBOs persist across frames and are written with LoadOp::Load
			// + alpha blend; zero them so a transparent scene background (e.g. the
			// editor's wireframe draw mode) doesn't blend against the previous frame
			// and accumulate. Opaque scenes overwrite these regardless.
			ClearViewTransparent(ViewOf(m_Impl->PingPongA));
			ClearViewTransparent(ViewOf(m_Impl->PingPongB));
		}

		float currentTime = 0.0f;
		if (Application* app = Application::GetInstance()) {
			currentTime = app->GetTime().GetRealtimeSinceStartup();
		}

		const wgpu::TextureFormat ppFormat = wgpu::TextureFormat::RGBA16Float;

		wgpu::TextureView curSrcView = ViewOf(src);
		if (!curSrcView) {
			IDX_CORE_WARN_TAG("PostProcessor", "Source FBO has no color view");
			return;
		}

		int pp = 0;

		auto dispatch = [&](EffectKind kind,
			wgpu::TextureView passSrc, wgpu::TextureView passDst,
			wgpu::TextureFormat passFmt, uint32_t passW, uint32_t passH)
		{
			switch (kind) {
				case EffectKind::Bloom:
					RunBloomPass(*m_Impl,
						*m_BloomThresholdShader, *m_BloomBlurShader, *m_BloomCompositeShader,
						passSrc, passDst, passFmt, passW, passH, settings->Bloom);
					break;
				case EffectKind::ColorGrading:
					RunColorGradingPass(*m_Impl, *m_ColorGradingShader,
						passSrc, passDst, passFmt, settings->ColorGrading);
					break;
				case EffectKind::LensDistortion:
					RunLensDistortionPass(*m_Impl, *m_LensDistortionShader,
						passSrc, passDst, passFmt, settings->LensDistortion);
					break;
				case EffectKind::Chromatic:
					RunChromaticPass(*m_Impl, *m_ChromaticShader,
						passSrc, passDst, passFmt, settings->ChromaticAberration);
					break;
				case EffectKind::Vignette:
					RunVignettePass(*m_Impl, *m_VignetteShader,
						passSrc, passDst, passFmt, passW, passH, settings->Vignette);
					break;
				case EffectKind::Grain:
					RunGrainPass(*m_Impl, *m_GrainShader,
						passSrc, passDst, passFmt, settings->Grain, currentTime);
					break;
				case EffectKind::Pixelated:
					RunPixelatedPass(*m_Impl, *m_PixelatedShader,
						passSrc, passDst, passFmt, passW, passH, settings->Pixelated);
					break;
				case EffectKind::GaussianBlur:
					RunGaussianBlurPass(*m_Impl, *m_BloomBlurShader,
						passSrc, passDst, passFmt, passW, passH, settings->GaussianBlur);
					break;
			}
		};

		for (size_t i = 0; i < enabledCount; ++i) {
			const bool isLast = (i + 1 == enabledCount);

			wgpu::TextureView passDstView;
			wgpu::TextureFormat passDstFormat;
			uint32_t passDstW;
			uint32_t passDstH;

			if (isLast) {
				passDstView   = dstView;
				passDstFormat = dstFormat;
				passDstW      = dstWidth;
				passDstH      = dstHeight;
			} else {
				Framebuffer& fb = (pp == 0) ? m_Impl->PingPongA : m_Impl->PingPongB;
				passDstView   = ViewOf(fb);
				passDstFormat = ppFormat;
				passDstW      = dstWidth;
				passDstH      = dstHeight;
				if (!passDstView) {
					IDX_CORE_WARN_TAG("PostProcessor",
						"Ping-pong FBO {} has no color view at pass {}", pp, i);
					return;
				}
			}

			dispatch(enabled[i], curSrcView, passDstView, passDstFormat, passDstW, passDstH);

			if (!isLast) {
				curSrcView = passDstView;
				pp = 1 - pp;
			}
		}
	}

} // namespace Index
