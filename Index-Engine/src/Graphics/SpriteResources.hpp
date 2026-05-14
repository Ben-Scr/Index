#pragma once

#include "Core/Export.hpp"
#include "Graphics/Instance44.hpp"

#include <webgpu/webgpu_cpp.h>

#include <cstdint>

// =============================================================================
// WebGPUSpriteResources — shared WebGPU state for the sprite + UI submit paths.
// -----------------------------------------------------------------------------
// Both Renderer2D (world-space sprites) and GuiRenderer (screen-space UI quads)
// pull their core GPU state from here so engine.dll owns one copy of:
//   * The sprite WGSL ShaderModule (vs_main / fs_main entry points)
//   * The unit-quad vertex + index buffers (4 verts, 6 indices)
//   * The BindGroupLayout + PipelineLayout for { u_ViewProj, t_albedo, s_albedo }
//   * A per-color-format wgpu::RenderPipeline cache (one pipeline per swap-chain
//     format / FBO format the engine actually renders to in a session, lazily
//     built on first request)
//
// Lifecycle is reference-counted: Acquire() in each renderer's Initialize,
// Release() in each renderer's Shutdown. The counter exists so engine.dll
// holds the resources exactly once even when multiple renderers init
// independently in different orders.
//
// Why this isn't in WebGPUBackend.hpp: that header is for cross-cutting
// resource-pool access (texture lookup, FBO lookup) used by many TUs. The
// sprite pipeline is specific to the sprite/UI submit path, so its scope
// matches Renderer2D + GuiRenderer rather than every WebGPU consumer.
// =============================================================================

namespace Index::WebGPUSpriteResources {

	enum class SpritePipelineMode : std::uint8_t {
		Filled = 0,
		Wireframe = 1
	};

	// Per-instance layout written into the instance VBO.
	struct INDEX_API SpriteInstance {
		float Pos[2];        // matches i_data0.xy in vs_main
		float Scale[2];      // matches i_data0.zw
		float Color[4];      // matches i_data1
		float Rot[4];        // matches i_data2 = (cos, sin, 0, 0)
	};

	// Reference-counted lifecycle. Returns false on shader-load failure —
	// callers can keep going without the sprite path (renderer falls back
	// to issuing no draws this frame; the engine doesn't crash). IsReady()
	// is the runtime "should I emit sprite draws this frame" check.
	INDEX_API bool Acquire();
	INDEX_API void Release();
	INDEX_API bool IsReady();

	// Submit-side accessors. All valid only between Acquire / Release.

	INDEX_API wgpu::ShaderModule GetSpriteModule();

	// Quad geometry — 4 vertices at [-0.5, 0.5]² (Z=0), 6 indices forming
	// two CCW triangles. Vertex stride = 12 bytes (3 floats).
	INDEX_API wgpu::Buffer GetQuadVertexBuffer();
	INDEX_API wgpu::Buffer GetQuadIndexBuffer(SpritePipelineMode mode = SpritePipelineMode::Filled);
	INDEX_API std::uint32_t GetQuadIndexCount(SpritePipelineMode mode = SpritePipelineMode::Filled);

	// Bind group / pipeline layout.
	//   group 0:
	//     binding 0: uniform buffer (mat4 viewProj)
	//     binding 1: 2D sampled texture
	//     binding 2: sampler
	INDEX_API wgpu::BindGroupLayout GetBindGroupLayout();
	INDEX_API wgpu::PipelineLayout  GetPipelineLayout();

	// Per-target-format render pipeline. Cached internally: a session
	// typically only renders to ~2 formats (swap-chain BGRA8Unorm or
	// BGRA8UnormSrgb, plus FBO RGBA8Unorm for the editor's panels), so
	// the cache stays small. `hasDepth` selects between the
	// no-depth-attachment variant (swap-chain Stage 1) and the
	// D24S8-depth variant (every editor FBO).
	INDEX_API wgpu::RenderPipeline GetSpritePipeline(
		wgpu::TextureFormat colorFormat,
		bool                hasDepth,
		SpritePipelineMode  mode = SpritePipelineMode::Filled);

	// CPU-side encode helper. Out-parameter so callers can write directly
	// into a transient instance buffer's mapped range with a single
	// memcpy-free assignment.
	INDEX_API void EncodeInstance44(const Instance44& src, SpriteInstance& dst);

}  // namespace Index::WebGPUSpriteResources
