#pragma once

#include "Core/Export.hpp"
#include "Graphics/Instance44.hpp"

#include <webgpu/webgpu_cpp.h>

#include <cstdint>


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
		float Rot[4];        // matches i_data2 = (rotation, 0, 0, 0)
		float Uv[4];         // matches i_data3 = (u0, v0, u1, v1) — sprite slice rect
		float MaskRect[4];   // matches i_data4 = (minX, minY, invW, invH)
		float MaskRot[4];    // matches i_data5 = (pivotX, pivotY, cos(-rot), sin(-rot))
		float MaskUv[4];     // matches i_data6 = (u0, v0, u1, v1)
	};

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

	INDEX_API wgpu::BindGroupLayout GetBindGroupLayout();
	INDEX_API wgpu::PipelineLayout  GetPipelineLayout();

	INDEX_API wgpu::RenderPipeline GetSpritePipeline(
		wgpu::TextureFormat colorFormat,
		bool                hasDepth,
		SpritePipelineMode  mode = SpritePipelineMode::Filled);

	INDEX_API void EncodeInstance44(const Instance44& src, SpriteInstance& dst);

}  // namespace Index::WebGPUSpriteResources
