#pragma once

// WebGPUBackend — internal wgpu Device/Queue/surface access for resource TUs (Texture2D, Framebuffer, Shader, etc.).
// Pool storage lives in each resource's .cpp TU; this header is the lookup surface only. Include RenderApi.hpp for backend-neutral types.

#include <webgpu/webgpu_cpp.h>

#include <cstdint>

namespace Index::WebGPUBackend {

	// ── Frame lifecycle (defined in Backend/WebGPUApi.cpp) ───────────────────

	// Submit the frame and present; issues a touch-clear when nothing was rendered so the swap chain still advances.
	void Present();

	wgpu::Device        GetDevice();
	wgpu::Queue         GetQueue();
	wgpu::TextureFormat GetSurfaceFormat();
	bool                IsInitialized();

	// False on Metal/older Vulkan; GpuTimer shows "N/A" when false.
	bool                HasTimestampQuery();

	// Currently always false — baseline WebGPU has no stable bindless feature; hook exists for future enablement.
	bool                HasBindlessTextures();

	// ── Texture2D pool (defined in Graphics/Texture2D_WebGPU.cpp) ────────────

	// backendId is the raw WGPUTextureView pointer so it doubles as ImTextureID for imgui_impl_wgpu.
	struct TextureLookup {
		wgpu::TextureView View;
		wgpu::Sampler     Sampler;
		uint32_t          Width  = 0;
		uint32_t          Height = 0;
		bool              Valid  = false;
	};
	TextureLookup LookupTexture2D(uint64_t backendId);

	// ── Framebuffer pool (defined in Graphics/Framebuffer_WebGPU.cpp) ────────

	struct FramebufferLookup {
		wgpu::TextureView ColorView;
		wgpu::TextureView DepthView;
		wgpu::TextureFormat ColorFormat = wgpu::TextureFormat::Undefined;
		uint32_t          Width  = 0;
		uint32_t          Height = 0;
		bool              Valid  = false;
	};
	FramebufferLookup LookupFramebufferByFboId(uint32_t fboBackendId);

	// colorTextureViewPtr IS a WGPUTextureView pointer; non-ImGui consumers use this to get the wgpu wrapper back.
	wgpu::TextureView LookupFramebufferColorViewByTextureId(uint64_t colorTextureViewPtr);

	enum class FilterMode : uint8_t { Point, Bilinear, Trilinear, Anisotropic };
	enum class WrapMode   : uint8_t { Repeat, Clamp, Mirror, Border };
	wgpu::Sampler CreateSampler(FilterMode filter, WrapMode wrapU, WrapMode wrapV);

	// ── Shader pool (defined in Graphics/Shader_WebGPU.cpp) ──────────────────

	struct ShaderLookup {
		wgpu::ShaderModule Module;
		bool               Valid = false;
	};
	ShaderLookup LookupShader(unsigned shaderHandleId);

	// ── Font atlas pool (defined in Graphics/Text/Font_WebGPU.cpp) ───────────

	wgpu::TextureView LookupFontAtlas(unsigned atlasId);

	// ── Render-pass plumbing (defined in Backend/WebGPUApi.cpp) ──────────────

	struct CurrentTargetInfo {
		wgpu::TextureView   ColorView;
		wgpu::TextureView   DepthView;
		wgpu::TextureFormat ColorFormat = wgpu::TextureFormat::Undefined;
		uint32_t            Width      = 0;
		uint32_t            Height     = 0;
		bool                IsSwapChain= true;
		bool                HasDepth   = false;
		bool                Valid      = false; // false = backend can't render this frame; caller must skip submit.
	};

	// Returns Valid=false on failure — caller must skip submit rather than open a pass on a null view.
	CurrentTargetInfo BeginRenderToCurrentTarget();

	// MUST be called after BeginRenderPass on every engine pass (WebGPU viewport is per-pass).
	// Do NOT call from ImGui passes — ImGui drives its own viewport per draw command.
	void ApplyCachedViewportToPass(wgpu::RenderPassEncoder& pass);

	// RestoreBoundTarget flushes commands so WriteBuffer copies to the intermediate target resolve before the next pass.
	struct BoundTargetSnapshot {
		wgpu::TextureView   ColorView;
		wgpu::TextureView   DepthView;
		wgpu::TextureFormat ColorFormat = wgpu::TextureFormat::Undefined;
		uint32_t            Width       = 0;
		uint32_t            Height      = 0;
		bool                IsSwapChain = true;
	};
	BoundTargetSnapshot SaveBoundTarget();
	void RestoreBoundTarget(const BoundTargetSnapshot& snap);

	wgpu::CommandEncoder GetFrameEncoder(); // lazy-created; null when uninitialised.

	// MUST be called when a pass targets IsSwapChain==true; prevents Present() from issuing a double-clear.
	void MarkSwapChainRendered();

	// MUST be called between sequences that share a WriteBuffer target: Dawn collapses multiple WriteBuffer calls
	// to the same buffer into the last value, so the first pass would read stale data without an explicit flush.
	void FlushCommands();

	// ── Multi-viewport surface support ───────────────────────────────────

	struct ViewportSurface;

	// Windows-only (HWND + HINSTANCE); returns nullptr on failure.
	ViewportSurface* CreateViewportSurface(void* hwnd, void* hinstance,
		uint32_t width, uint32_t height);

	void DestroyViewportSurface(ViewportSurface* vs); // safe with null.
	void ResizeViewportSurface(ViewportSurface* vs,
		uint32_t width, uint32_t height);

	// Returns null on failure; caller must skip rendering but still call PresentViewportSurface (no-op on null).
	wgpu::TextureView AcquireViewportSurfaceView(ViewportSurface* vs);

	void PresentViewportSurface(ViewportSurface* vs);

	// Matches the main swap-chain format so one ImGui_ImplWGPU pipeline covers all viewport surfaces.
	wgpu::TextureFormat GetViewportSurfaceFormat();

	wgpu::Instance GetInstance(); // do not Release — engine owns lifetime.

}  // namespace Index::WebGPUBackend
