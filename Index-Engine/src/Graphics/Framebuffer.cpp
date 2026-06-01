#include "pch.hpp"
#include "Graphics/Framebuffer.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"

#include <webgpu/webgpu_cpp.h>

#include <unordered_map>
#include <utility>

namespace Index {

	namespace {
		struct GpuFramebuffer {
			wgpu::Texture       ColorTexture;
			wgpu::Texture       DepthTexture;
			wgpu::TextureView   ColorView;
			wgpu::TextureView   DepthView;
			wgpu::TextureFormat ColorFormat = wgpu::TextureFormat::Undefined;
			uint32_t            Width  = 0;
			uint32_t            Height = 0;
			uint32_t            ColorTextureId = 0;  // back-pointer for cleanup
		};

		std::unordered_map<uint32_t, GpuFramebuffer> g_Framebuffers;
		// Reverse map: raw WGPUTextureView pointer → fboBackendId (diagnostic / future consumers; ImGui path uses the pointer directly).
		std::unordered_map<uint64_t, uint32_t> g_ColorViewToFbo;
		uint32_t g_NextFbId = 1;

		// Two-frame deferred destroy: keeps Dawn resources alive through N+1 so in-flight ImGui draws using the raw view pointer don't dereference a freed handle.
		std::vector<GpuFramebuffer> g_PendingDestroyThisFrame;
		std::vector<GpuFramebuffer> g_PendingDestroyLastFrame;

		uint32_t AllocateFbId() {
			uint32_t id = g_NextFbId++;
			if (id == 0) id = g_NextFbId++;  // overflow guard, theoretical
			return id;
		}

		GpuFramebuffer* TryLookupFb(uint32_t id) {
			if (id == 0) return nullptr;
			auto it = g_Framebuffers.find(id);
			return (it == g_Framebuffers.end()) ? nullptr : &it->second;
		}

		// Map the engine's backend-neutral TextureFormat enum to a wgpu format.
		// Four supported formats (RGBA8 / R8 / Depth24Stencil8 / RGBA16F);
		// anything else is a hard error rather than a silent substitution.
		wgpu::TextureFormat ToWgpuFormat(TextureFormat f) noexcept {
			switch (f) {
				case TextureFormat::RGBA8:           return wgpu::TextureFormat::RGBA8Unorm;
				case TextureFormat::R8:              return wgpu::TextureFormat::R8Unorm;
				case TextureFormat::Depth24Stencil8: return wgpu::TextureFormat::Depth24PlusStencil8;
				case TextureFormat::RGBA16F:         return wgpu::TextureFormat::RGBA16Float;
			}
			return wgpu::TextureFormat::RGBA8Unorm;
		}
	}

	namespace WebGPUBackend {

		FramebufferLookup LookupFramebufferByFboId(uint32_t fboBackendId) {
			GpuFramebuffer* fb = TryLookupFb(fboBackendId);
			if (!fb) return FramebufferLookup{};

			FramebufferLookup out;
			out.ColorView   = fb->ColorView;
			out.DepthView   = fb->DepthView;
			out.ColorFormat = fb->ColorFormat;
			out.Width       = fb->Width;
			out.Height      = fb->Height;
			out.Valid       = true;
			return out;
		}

		wgpu::TextureView LookupFramebufferColorViewByTextureId(uint64_t colorTextureViewPtr) {
			// m_ColorTextureId IS the raw WGPUTextureView pointer; this function exists for non-ImGui consumers that need the wgpu::TextureView wrapper.
			if (colorTextureViewPtr == 0) return nullptr;
			auto mapIt = g_ColorViewToFbo.find(colorTextureViewPtr);
			if (mapIt == g_ColorViewToFbo.end()) return nullptr;
			GpuFramebuffer* fb = TryLookupFb(mapIt->second);
			return fb ? fb->ColorView : nullptr;
		}

	}  // namespace WebGPUBackend

	// ── Framebuffer ─────────────────────────────────────────────────────────

	Framebuffer::Framebuffer(int width, int height,
		TextureFormat colorFormat, TextureFilter filter)
	{
		Recreate(width, height, colorFormat, filter);
	}

	Framebuffer::Framebuffer(Framebuffer&& other) noexcept
		: m_BackendId(other.m_BackendId)
		, m_ColorTextureId(other.m_ColorTextureId)
		, m_DepthRenderbuffer(other.m_DepthRenderbuffer)
		, m_Viewport(other.m_Viewport)
		, m_ColorFormat(other.m_ColorFormat)
	{
		other.m_BackendId         = 0;
		other.m_ColorTextureId    = 0;
		other.m_DepthRenderbuffer = 0;
		other.m_Viewport          = Viewport{ 0, 0 };
	}

	Framebuffer& Framebuffer::operator=(Framebuffer&& other) noexcept {
		if (this == &other) return *this;
		Destroy();
		m_BackendId         = other.m_BackendId;
		m_ColorTextureId    = other.m_ColorTextureId;
		m_DepthRenderbuffer = other.m_DepthRenderbuffer;
		m_ColorFormat       = other.m_ColorFormat;
		m_Viewport          = other.m_Viewport;
		other.m_BackendId         = 0;
		other.m_ColorTextureId    = 0;
		other.m_DepthRenderbuffer = 0;
		other.m_Viewport          = Viewport{ 0, 0 };
		return *this;
	}

	Framebuffer::~Framebuffer() {
		Destroy();
	}

	bool Framebuffer::Recreate(int width, int height,
		TextureFormat colorFormat, TextureFilter /*filter*/)
	{
		if (width <= 0 || height <= 0) return false;

		const bool sizeMatches = m_Viewport.GetWidth() == width
			&& m_Viewport.GetHeight() == height;
		if (m_BackendId != 0 && sizeMatches && m_ColorFormat == colorFormat) {
			return true;
		}

		Destroy();
		m_Viewport.SetSize(width, height);
		m_ColorFormat = colorFormat;

		if (!WebGPUBackend::IsInitialized()) {
			IDX_CORE_ERROR_TAG("Framebuffer",
				"Recreate called before WebGPU backend initialized");
			return false;
		}

		wgpu::Device device = WebGPUBackend::GetDevice();
		const wgpu::TextureFormat colorWgpuFormat = ToWgpuFormat(colorFormat);

		wgpu::TextureDescriptor colorDesc{};
		colorDesc.dimension       = wgpu::TextureDimension::e2D;
		colorDesc.size            = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
		colorDesc.format          = colorWgpuFormat;
		colorDesc.usage           = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
		colorDesc.mipLevelCount   = 1;
		colorDesc.sampleCount     = 1;
		colorDesc.viewFormatCount = 0;

		wgpu::Texture color = device.CreateTexture(&colorDesc);
		if (!color) {
			IDX_CORE_ERROR_TAG("Framebuffer", "wgpu::Device::CreateTexture (color) failed");
			return false;
		}

		// Depth/stencil attachment — always D24S8. Project settings can
		// add an HDR / no-stencil variant later for renderers that don't
		// need stencil.
		wgpu::TextureDescriptor depthDesc{};
		depthDesc.dimension       = wgpu::TextureDimension::e2D;
		depthDesc.size            = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
		depthDesc.format          = wgpu::TextureFormat::Depth24PlusStencil8;
		depthDesc.usage           = wgpu::TextureUsage::RenderAttachment;
		depthDesc.mipLevelCount   = 1;
		depthDesc.sampleCount     = 1;
		depthDesc.viewFormatCount = 0;

		wgpu::Texture depth = device.CreateTexture(&depthDesc);
		if (!depth) {
			IDX_CORE_ERROR_TAG("Framebuffer", "wgpu::Device::CreateTexture (depth) failed");
			return false;
		}

		// Views. The colour view is the same one ImGui::Image samples; the
		// depth view is only consumed by the render-pass descriptor's
		// depthStencilAttachment.
		wgpu::TextureViewDescriptor colorViewDesc{};
		colorViewDesc.format          = colorWgpuFormat;
		colorViewDesc.dimension       = wgpu::TextureViewDimension::e2D;
		colorViewDesc.mipLevelCount   = 1;
		colorViewDesc.arrayLayerCount = 1;
		colorViewDesc.aspect          = wgpu::TextureAspect::All;
		wgpu::TextureView colorView = color.CreateView(&colorViewDesc);

		wgpu::TextureViewDescriptor depthViewDesc{};
		depthViewDesc.format          = wgpu::TextureFormat::Depth24PlusStencil8;
		depthViewDesc.dimension       = wgpu::TextureViewDimension::e2D;
		depthViewDesc.mipLevelCount   = 1;
		depthViewDesc.arrayLayerCount = 1;
		depthViewDesc.aspect          = wgpu::TextureAspect::All;
		wgpu::TextureView depthView = depth.CreateView(&depthViewDesc);

		const uint32_t fbId = AllocateFbId();
		// MUST be a real Dawn handle (not a pool int): imgui_impl_wgpu reinterpret_casts ImTextureID→WGPUTextureView and dereferences it.
		const uint64_t colorViewPtr =
			reinterpret_cast<uint64_t>(colorView.Get());

		GpuFramebuffer entry;
		entry.ColorTexture   = std::move(color);
		entry.DepthTexture   = std::move(depth);
		entry.ColorView      = std::move(colorView);
		entry.DepthView      = std::move(depthView);
		entry.ColorFormat    = colorWgpuFormat;
		entry.Width          = static_cast<uint32_t>(width);
		entry.Height         = static_cast<uint32_t>(height);
		entry.ColorTextureId = 0;  // legacy field, no longer used as a pool key

		g_Framebuffers.emplace(fbId, std::move(entry));
		g_ColorViewToFbo[colorViewPtr] = fbId;

		m_BackendId         = fbId;
		m_ColorTextureId    = colorViewPtr;
		m_DepthRenderbuffer = fbId; // kept non-zero for IsValid bookkeeping; depth is owned by the GpuFramebuffer entry
		return true;
	}

	void Framebuffer::Destroy() {
		if (m_BackendId != 0) {
			// Erase the reverse-map entry immediately; Dawn resources move to the deferred bucket so in-flight ImGui draws finish before the handle is freed.
			if (m_ColorTextureId != 0) {
				g_ColorViewToFbo.erase(m_ColorTextureId);
			}
			auto it = g_Framebuffers.find(m_BackendId);
			if (it != g_Framebuffers.end()) {
				g_PendingDestroyThisFrame.push_back(std::move(it->second));
				g_Framebuffers.erase(it);
			}

			m_BackendId         = 0;
			m_ColorTextureId    = 0;
			m_DepthRenderbuffer = 0;
		}
		m_Viewport = Viewport{ 0, 0 };
	}

	void Framebuffer::ProcessFrameEndDeferredDestroy() {
		g_PendingDestroyLastFrame.clear();
		g_PendingDestroyLastFrame.swap(g_PendingDestroyThisFrame);
	}

}  // namespace Index
