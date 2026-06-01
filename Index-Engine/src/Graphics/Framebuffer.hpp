#pragma once

#include "Collections/Viewport.hpp"
#include "Core/Export.hpp"
#include "Graphics/RenderApiTypes.hpp"

#include <cstdint>

// Framebuffer: RAII offscreen render target used by editor panel viewports. GetColorTextureBackendId() returns the raw WGPUTextureView pointer as uint64_t for ImGui::Image.

namespace Index {

	class INDEX_API Framebuffer {
	public:
		Framebuffer() = default;
		Framebuffer(int width, int height,
			TextureFormat colorFormat = TextureFormat::RGBA8,
			TextureFilter filter = TextureFilter::Linear);

		// Move-only — backend handles are unique resources; copying would
		// double-free on destruction.
		Framebuffer(const Framebuffer&) = delete;
		Framebuffer& operator=(const Framebuffer&) = delete;
		Framebuffer(Framebuffer&& other) noexcept;
		Framebuffer& operator=(Framebuffer&& other) noexcept;

		~Framebuffer();

		bool Recreate(int width, int height,
			TextureFormat colorFormat = TextureFormat::RGBA8,
			TextureFilter filter = TextureFilter::Linear);

		// Dawn resources are NOT freed immediately — moved to a two-frame deferred-destroy queue so ImGui frames still holding the WGPUTextureView pointer finish before the handle releases.
		void Destroy();

		static void ProcessFrameEndDeferredDestroy();

		bool IsValid() const { return m_BackendId != 0; }

		// Backend identifier for the framebuffer object itself. Required by
		// `RenderApi::BindFramebuffer`; callers shouldn't interpret the
		// integer otherwise.
		uint32_t GetBackendId() const { return m_BackendId; }

		// Returns raw WGPUTextureView pointer as uint64_t — imgui_impl_wgpu casts ImTextureID back to WGPUTextureView and dereferences it directly.
		uint64_t GetColorTextureBackendId() const { return m_ColorTextureId; }

		int GetWidth() const { return m_Viewport.GetWidth(); }
		int GetHeight() const { return m_Viewport.GetHeight(); }
		const Viewport& GetViewport() const { return m_Viewport; }

	private:
		// m_ColorTextureId stores raw WGPUTextureView pointer (uint64_t) so ImGui passes it straight to imgui_impl_wgpu without a pool lookup. m_BackendId is a 32-bit Framebuffer_WebGPU.cpp pool key.
		uint32_t m_BackendId       = 0;
		uint64_t m_ColorTextureId  = 0;
		uint32_t m_DepthRenderbuffer = 0;
		// Tracked so Recreate's same-size fast-path also catches FORMAT changes (e.g. RGBA8 -> RGBA16F at equal size).
		TextureFormat m_ColorFormat = TextureFormat::RGBA8;

		Viewport m_Viewport{ 0, 0 };
	};

} // namespace Index
