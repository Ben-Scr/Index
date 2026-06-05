#include "pch.hpp"
#include "Graphics/RenderApi.hpp"

#include "Core/Application.hpp"
#include "Core/Log.hpp"
#include "Core/Window.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/Framebuffer.hpp"
#include "Graphics/GLInitSpecifications.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"

#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
// glfw3native.h pulls in <windows.h> for glfwGetWin32Window; only the Windows
// surface path needs it (Linux/X11 surface is Stage 1b). Keep it Windows-only.
#if defined(IDX_PLATFORM_WINDOWS)
    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3native.h>
#endif

#if defined(IDX_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <Windows.h>
#endif


namespace Index {

	namespace {
		// ── Backend lifecycle state ─────────────────────────────────────────
		bool        g_Initialized = false;
		std::string g_VersionString;
		std::string g_VendorString;
		std::string g_RendererString;

		Color    g_ClearColor{ 0.0f, 0.0f, 0.0f, 1.0f };
		uint32_t g_BackbufferWidth = 0;
		uint32_t g_BackbufferHeight = 0;
		bool     g_VsyncEnabled = true;

		// ── WebGPU objects ──────────────────────────────────────────────────
		wgpu::Instance      g_Instance;
		wgpu::Adapter       g_Adapter;
		wgpu::Device        g_Device;
		wgpu::Queue         g_Queue;
		wgpu::Surface       g_Surface;
		wgpu::TextureFormat g_SurfaceFormat = wgpu::TextureFormat::Undefined;

		bool g_HasTimestampQuery = false;

		// Always false until Dawn stabilises a portable bindless feature name; gate kept ready for when it does.
		bool g_HasBindlessTextures = false;

		// ── Per-frame transient state ───────────────────────────────────────
		struct FrameState {
			wgpu::CommandEncoder    Encoder;
			wgpu::Texture           SurfaceTexture;
			wgpu::TextureView       SurfaceView;
			wgpu::RenderPassEncoder ActivePass;
			bool HasEncoder = false;
			bool HasSurfaceTexture = false;
			bool HasActivePass = false;
			bool PresentedSwapChain = false;
		};
		FrameState g_Frame;

		struct TargetState {
			wgpu::TextureView ColorView;       // null -> use swap chain's per-frame view
			wgpu::TextureView DepthView;       // null -> no depth attachment
			wgpu::TextureFormat ColorFormat = wgpu::TextureFormat::Undefined;
			bool IsSwapChain = true;
			uint32_t Width  = 0;
			uint32_t Height = 0;
		};
		TargetState g_CurrentTarget;

		uint32_t g_ViewportX = 0, g_ViewportY = 0, g_ViewportW = 0, g_ViewportH = 0;
		uint32_t g_ScissorX  = 0, g_ScissorY  = 0, g_ScissorW  = 0, g_ScissorH  = 0;
		bool     g_ScissorActive = false;
		PolygonMode g_PolygonMode = PolygonMode::Filled;

		// ── Helpers ─────────────────────────────────────────────────────────

		const char* AdapterTypeName(wgpu::AdapterType t) {
			switch (t) {
				case wgpu::AdapterType::DiscreteGPU:   return "DiscreteGPU";
				case wgpu::AdapterType::IntegratedGPU: return "IntegratedGPU";
				case wgpu::AdapterType::CPU:           return "CPU";
				default:                               return "Unknown";
			}
		}

		const char* BackendTypeName(wgpu::BackendType t) {
			switch (t) {
				case wgpu::BackendType::D3D11:   return "Direct3D11";
				case wgpu::BackendType::D3D12:   return "Direct3D12";
				case wgpu::BackendType::Vulkan:  return "Vulkan";
				case wgpu::BackendType::Metal:   return "Metal";
				case wgpu::BackendType::OpenGL:  return "OpenGL";
				case wgpu::BackendType::OpenGLES:return "OpenGLES";
				case wgpu::BackendType::Null:    return "Null";
				default:                         return "Unknown";
			}
		}

		bool SupportsPresentMode(const wgpu::SurfaceCapabilities& caps, wgpu::PresentMode mode) {
			for (size_t i = 0; i < caps.presentModeCount; ++i) {
				if (caps.presentModes[i] == mode) return true;
			}
			return false;
		}

		wgpu::PresentMode ChoosePresentMode(const wgpu::SurfaceCapabilities& caps) {
			if (g_VsyncEnabled) {
				if (SupportsPresentMode(caps, wgpu::PresentMode::Fifo)) {
					return wgpu::PresentMode::Fifo;
				}
				return caps.presentModeCount > 0 ? caps.presentModes[0] : wgpu::PresentMode::Fifo;
			}

			if (SupportsPresentMode(caps, wgpu::PresentMode::Immediate)) {
				return wgpu::PresentMode::Immediate;
			}
			if (SupportsPresentMode(caps, wgpu::PresentMode::Mailbox)) {
				return wgpu::PresentMode::Mailbox;
			}
			return caps.presentModeCount > 0 ? caps.presentModes[0] : wgpu::PresentMode::Fifo;
		}

		std::string FromStringView(wgpu::StringView sv) {
			if (sv.data == nullptr || sv.length == 0) return {};
			return std::string(sv.data, sv.length);
		}

		void PumpEvents() {
			if (g_Instance) g_Instance.ProcessEvents();
		}

		bool CreateSurface() {
			Window* win = Application::GetWindow();
			if (!win) {
				IDX_CORE_ERROR_TAG("WebGPUApi", "No engine window available for surface creation");
				return false;
			}
			GLFWwindow* w = win->GetGLFWWindow();
			if (!w) {
				IDX_CORE_ERROR_TAG("WebGPUApi", "GLFW window handle is null");
				return false;
			}

#if defined(IDX_PLATFORM_WINDOWS)
			wgpu::SurfaceSourceWindowsHWND hwndSource{};
			hwndSource.hwnd      = glfwGetWin32Window(w);
			hwndSource.hinstance = ::GetModuleHandleW(nullptr);

			wgpu::SurfaceDescriptor surfaceDesc{};
			surfaceDesc.nextInChain = &hwndSource;

			g_Surface = g_Instance.CreateSurface(&surfaceDesc);
#else
			IDX_CORE_ERROR_TAG("WebGPUApi",
				"WebGPU backend platform-data is Windows-only in Stage 1 (macOS NSView + Linux X11/Wayland land in Stage 1b)");
			return false;
#endif
			if (!g_Surface) {
				IDX_CORE_ERROR_TAG("WebGPUApi", "wgpu::Instance::CreateSurface returned null");
				return false;
			}
			return true;
		}

		// Returns Undefined (let Dawn pick) for Auto or when no project is loaded yet.
		wgpu::BackendType PreferredBackendType() {
			IndexProject* project = ProjectManager::GetCurrentProject();
			if (!project) return wgpu::BackendType::Undefined;
			switch (project->ActiveRenderBackend) {
				case IndexProject::RenderBackend::Auto:       return wgpu::BackendType::Undefined;
				case IndexProject::RenderBackend::Vulkan:     return wgpu::BackendType::Vulkan;
				case IndexProject::RenderBackend::Direct3D11: return wgpu::BackendType::D3D11;
				case IndexProject::RenderBackend::Direct3D12: return wgpu::BackendType::D3D12;
				case IndexProject::RenderBackend::OpenGL:     return wgpu::BackendType::OpenGL;
				case IndexProject::RenderBackend::Metal:      return wgpu::BackendType::Metal;
				case IndexProject::RenderBackend::OpenGLES:   return wgpu::BackendType::OpenGLES;
			}
			return wgpu::BackendType::Undefined;
		}

		bool TryRequestAdapter(wgpu::BackendType backendType,
			wgpu::Adapter& outAdapter, std::string& outError)
		{
			wgpu::RequestAdapterOptions opts{};
			opts.compatibleSurface = g_Surface;
			opts.powerPreference   = wgpu::PowerPreference::HighPerformance;
			opts.backendType       = backendType;

			struct AdapterCtx { wgpu::Adapter Adapter; std::string Error; };
			AdapterCtx ctx;

			wgpu::Future future = g_Instance.RequestAdapter(
				&opts,
				wgpu::CallbackMode::WaitAnyOnly,
				[&ctx](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView msg) {
					if (status == wgpu::RequestAdapterStatus::Success) {
						ctx.Adapter = std::move(adapter);
					} else {
						ctx.Error = FromStringView(msg);
					}
				});

			wgpu::FutureWaitInfo wait{ future };
			g_Instance.WaitAny(1, &wait, /*timeoutNS*/ UINT64_MAX);

			if (!ctx.Adapter) {
				outError = std::move(ctx.Error);
				return false;
			}
			outAdapter = std::move(ctx.Adapter);
			return true;
		}

		bool RequestAdapterSync() {
			const wgpu::BackendType preferred = PreferredBackendType();

			wgpu::Adapter adapter;
			std::string   error;

			if (preferred != wgpu::BackendType::Undefined) {
				if (TryRequestAdapter(preferred, adapter, error)) {
					g_Adapter = std::move(adapter);
					return true;
				}
				IDX_CORE_WARN_TAG("WebGPUApi",
					"Requested {} backend not available on this host ({}); falling back to Auto.",
					BackendTypeName(preferred),
					error.empty() ? "no compatible GPU" : error);
			}

			if (TryRequestAdapter(wgpu::BackendType::Undefined, adapter, error)) {
				g_Adapter = std::move(adapter);
				return true;
			}

			IDX_CORE_ERROR_TAG("WebGPUApi", "RequestAdapter failed: {}",
				error.empty() ? "no compatible GPU" : error);
			return false;
		}

		bool RequestDeviceSync() {
			wgpu::DeviceDescriptor desc{};
			static wgpu::FeatureName s_RequestedFeatures[1] = {};
			uint32_t requestedFeatureCount = 0;
			if (g_Adapter && g_Adapter.HasFeature(wgpu::FeatureName::TimestampQuery)) {
				s_RequestedFeatures[requestedFeatureCount++] = wgpu::FeatureName::TimestampQuery;
			}
			desc.requiredFeatureCount = requestedFeatureCount;
			desc.requiredFeatures     = requestedFeatureCount > 0 ? s_RequestedFeatures : nullptr;

			desc.SetUncapturedErrorCallback(
				[](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
					const char* kind = "Unknown";
					switch (type) {
						case wgpu::ErrorType::Validation:  kind = "Validation";  break;
						case wgpu::ErrorType::OutOfMemory: kind = "OutOfMemory"; break;
						case wgpu::ErrorType::Internal:    kind = "Internal";    break;
						default: break;
					}
					IDX_CORE_ERROR_TAG("WebGPUApi", "WebGPU [{}]: {}",
						kind, FromStringView(msg));
				});
			desc.SetDeviceLostCallback(
				wgpu::CallbackMode::AllowSpontaneous,
				[](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
					if (reason == wgpu::DeviceLostReason::Destroyed) {
						return;
					}
					IDX_CORE_ERROR_TAG("WebGPUApi", "Device lost ({}): {}",
						static_cast<int>(reason), FromStringView(msg));
				});

			struct DeviceCtx { wgpu::Device Device; std::string Error; };
			DeviceCtx ctx;

			wgpu::Future future = g_Adapter.RequestDevice(
				&desc,
				wgpu::CallbackMode::WaitAnyOnly,
				[&ctx](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView msg) {
					if (status == wgpu::RequestDeviceStatus::Success) {
						ctx.Device = std::move(device);
					} else {
						ctx.Error = FromStringView(msg);
					}
				});

			wgpu::FutureWaitInfo wait{ future };
			g_Instance.WaitAny(1, &wait, UINT64_MAX);

			if (!ctx.Device) {
				IDX_CORE_ERROR_TAG("WebGPUApi", "RequestDevice failed: {}",
					ctx.Error.empty() ? "unknown" : ctx.Error);
				return false;
			}
			g_Device = std::move(ctx.Device);
			g_Queue  = g_Device.GetQueue();
			g_HasTimestampQuery = (requestedFeatureCount > 0);
			return true;
		}

		void ConfigureSurface(uint32_t width, uint32_t height) {
			wgpu::SurfaceCapabilities caps{};
			g_Surface.GetCapabilities(g_Adapter, &caps);

			g_SurfaceFormat = (caps.formatCount > 0)
				? caps.formats[0]
				: wgpu::TextureFormat::BGRA8Unorm;

			wgpu::SurfaceConfiguration config{};
			config.device      = g_Device;
			config.format      = g_SurfaceFormat;
			config.usage       = wgpu::TextureUsage::RenderAttachment;
			config.width       = width;
			config.height      = height;
			config.presentMode = ChoosePresentMode(caps);
			config.alphaMode   = wgpu::CompositeAlphaMode::Opaque;
			config.viewFormatCount = 0;

			g_Surface.Configure(&config);

			g_BackbufferWidth  = width;
			g_BackbufferHeight = height;

			// Initial render target = swap chain.
			g_CurrentTarget = TargetState{};
			g_CurrentTarget.IsSwapChain = true;
			g_CurrentTarget.Width  = width;
			g_CurrentTarget.Height = height;

			// Viewport defaults to full surface so callers don't have to
			// set one before the first Clear.
			g_ViewportX = 0; g_ViewportY = 0;
			g_ViewportW = width; g_ViewportH = height;
		}

		// ── Frame lifecycle ─────────────────────────────────────────────────

		void EnsureFrameEncoder() {
			if (g_Frame.HasEncoder) return;
			wgpu::CommandEncoderDescriptor desc{};
			g_Frame.Encoder = g_Device.CreateCommandEncoder(&desc);
			g_Frame.HasEncoder = true;
		}

		bool EnsureSurfaceTexture() {
			if (g_Frame.HasSurfaceTexture) return true;

			wgpu::SurfaceTexture surfaceTex{};
			g_Surface.GetCurrentTexture(&surfaceTex);

			// SuboptimalConfiguration is non-fatal — the surface still has a
			// usable texture, we just want to re-configure on next resize.
			const bool ok =
				surfaceTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
				surfaceTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal;
			if (!ok) {
				IDX_CORE_WARN_TAG("WebGPUApi",
					"GetCurrentTexture: status={} — skipping frame",
					static_cast<int>(surfaceTex.status));
				return false;
			}

			g_Frame.SurfaceTexture = wgpu::Texture(surfaceTex.texture);

			wgpu::TextureViewDescriptor viewDesc{};
			viewDesc.format          = g_SurfaceFormat;
			viewDesc.dimension       = wgpu::TextureViewDimension::e2D;
			viewDesc.mipLevelCount   = 1;
			viewDesc.arrayLayerCount = 1;
			viewDesc.aspect          = wgpu::TextureAspect::All;
			g_Frame.SurfaceView = g_Frame.SurfaceTexture.CreateView(&viewDesc);

			g_Frame.HasSurfaceTexture = true;
			return true;
		}

		void EndActivePassIfAny() {
			if (!g_Frame.HasActivePass) return;
			g_Frame.ActivePass.End();
			g_Frame.ActivePass = nullptr;
			g_Frame.HasActivePass = false;
		}

		// MUST submit between target switches: Dawn flushes ALL WriteBuffer calls before ANY pass executes,
		// so a second WriteBuffer(uniform) on the same buffer overwrites the first before the first pass runs.
		// Separate submissions give each FBO its own atomic WriteBuffer+pass unit.
		void FlushFrameCommands() {
			EndActivePassIfAny();
			if (!g_Frame.HasEncoder) return;

			wgpu::CommandBufferDescriptor cbDesc{};
			wgpu::CommandBuffer cmd = g_Frame.Encoder.Finish(&cbDesc);
			g_Queue.Submit(1, &cmd);

			g_Frame.Encoder    = nullptr;
			g_Frame.HasEncoder = false;
			// Surface texture/view and PresentedSwapChain stay alive — ImGui pass still needs them.
		}
	}  // anonymous namespace

	namespace WebGPUBackend {
		void Present() {
			if (!g_Initialized) return;

			if (!g_Frame.PresentedSwapChain) {
				EnsureFrameEncoder();
				if (EnsureSurfaceTexture()) {
					wgpu::RenderPassColorAttachment colorAtt{};
					colorAtt.view       = g_Frame.SurfaceView;
					colorAtt.loadOp     = wgpu::LoadOp::Clear;
					colorAtt.storeOp    = wgpu::StoreOp::Store;
					colorAtt.clearValue = wgpu::Color{
						g_ClearColor.r, g_ClearColor.g, g_ClearColor.b, g_ClearColor.a };
					colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

					wgpu::RenderPassDescriptor passDesc{};
					passDesc.colorAttachmentCount = 1;
					passDesc.colorAttachments     = &colorAtt;

					wgpu::RenderPassEncoder pass = g_Frame.Encoder.BeginRenderPass(&passDesc);
					pass.End();
					g_Frame.PresentedSwapChain = true;
				}
			} else {
				EndActivePassIfAny();
			}

			if (g_Frame.HasEncoder) {
				wgpu::CommandBufferDescriptor cbDesc{};
				wgpu::CommandBuffer cmd = g_Frame.Encoder.Finish(&cbDesc);
				g_Queue.Submit(1, &cmd);
			}

			if (g_Frame.HasSurfaceTexture) {
				g_Surface.Present();
			}

			Framebuffer::ProcessFrameEndDeferredDestroy();
			g_Frame = FrameState{};
			PumpEvents();
		}

		wgpu::Device GetDevice() { return g_Device; }
		wgpu::Queue  GetQueue()  { return g_Queue; }
		wgpu::TextureFormat GetSurfaceFormat() { return g_SurfaceFormat; }
		bool HasTimestampQuery() { return g_HasTimestampQuery; }
		bool HasBindlessTextures() { return g_HasBindlessTextures; }
		bool IsInitialized() { return g_Initialized; }

		wgpu::CommandEncoder GetFrameEncoder() {
			if (!g_Initialized) return nullptr;
			EnsureFrameEncoder();
			return g_Frame.Encoder;
		}

		void ApplyCachedViewportToPass(wgpu::RenderPassEncoder& pass) {
			if (!pass) return;
			if (g_ViewportW == 0 || g_ViewportH == 0) return; // Dawn validation errors on zero w/h
			pass.SetViewport(
				static_cast<float>(g_ViewportX),
				static_cast<float>(g_ViewportY),
				static_cast<float>(g_ViewportW),
				static_cast<float>(g_ViewportH),
				0.0f, 1.0f);
		}

		CurrentTargetInfo BeginRenderToCurrentTarget() {
			CurrentTargetInfo out;
			if (!g_Initialized) return out;

			EnsureFrameEncoder();

			if (g_CurrentTarget.IsSwapChain) {
				if (!EnsureSurfaceTexture()) return out;
				out.ColorView   = g_Frame.SurfaceView;
				out.DepthView   = nullptr;
				out.ColorFormat = g_SurfaceFormat;
				out.HasDepth    = false;
				out.IsSwapChain = true;
				out.Width       = g_BackbufferWidth;
				out.Height      = g_BackbufferHeight;
			} else {
				if (!g_CurrentTarget.ColorView) return out;
				out.ColorView   = g_CurrentTarget.ColorView;
				out.DepthView   = g_CurrentTarget.DepthView;
				out.ColorFormat = g_CurrentTarget.ColorFormat;
				out.HasDepth    = static_cast<bool>(g_CurrentTarget.DepthView);
				out.IsSwapChain = false;
				out.Width       = g_CurrentTarget.Width;
				out.Height      = g_CurrentTarget.Height;
			}

			EndActivePassIfAny();

			out.Valid = true;
			return out;
		}

		void MarkSwapChainRendered() {
			g_Frame.PresentedSwapChain = true;
		}

		void FlushCommands() {
			FlushFrameCommands();
		}

		BoundTargetSnapshot SaveBoundTarget() {
			BoundTargetSnapshot snap;
			snap.ColorView   = g_CurrentTarget.ColorView;
			snap.DepthView   = g_CurrentTarget.DepthView;
			snap.ColorFormat = g_CurrentTarget.ColorFormat;
			snap.Width       = g_CurrentTarget.Width;
			snap.Height      = g_CurrentTarget.Height;
			snap.IsSwapChain = g_CurrentTarget.IsSwapChain;
			return snap;
		}

		void RestoreBoundTarget(const BoundTargetSnapshot& snap) {
			FlushFrameCommands();

			g_CurrentTarget = TargetState{};
			g_CurrentTarget.ColorView   = snap.ColorView;
			g_CurrentTarget.DepthView   = snap.DepthView;
			g_CurrentTarget.ColorFormat = snap.ColorFormat;
			g_CurrentTarget.Width       = snap.Width;
			g_CurrentTarget.Height      = snap.Height;
			g_CurrentTarget.IsSwapChain = snap.IsSwapChain;
		}

		// ── Multi-viewport surface support ─────────────────────────────────
		// See WebGPUBackend.hpp for the rationale. Each ViewportSurface is
		// self-contained and presents independently of the main surface.

		struct ViewportSurface {
			wgpu::Surface            Surface;
			wgpu::Texture            CurrentTexture;
			wgpu::TextureView        CurrentView;
			uint32_t                 Width  = 0;
			uint32_t                 Height = 0;
			wgpu::PresentMode        PresentMode = wgpu::PresentMode::Fifo;
			wgpu::CompositeAlphaMode AlphaMode   = wgpu::CompositeAlphaMode::Opaque;
			bool                     HasCurrent  = false;
		};

		std::vector<ViewportSurface*> g_LiveViewportSurfaces;

		namespace {
			bool ConfigureViewportSurface(ViewportSurface* vs,
				uint32_t width, uint32_t height)
			{
				if (!vs || !vs->Surface || width == 0 || height == 0) return false;

				wgpu::SurfaceCapabilities caps{};
				vs->Surface.GetCapabilities(g_Adapter, &caps);

				vs->PresentMode = ChoosePresentMode(caps);
				vs->AlphaMode   = wgpu::CompositeAlphaMode::Opaque;
				for (size_t i = 0; i < caps.alphaModeCount; ++i) {
					if (caps.alphaModes[i] == wgpu::CompositeAlphaMode::Premultiplied) {
						vs->AlphaMode = wgpu::CompositeAlphaMode::Premultiplied;
						break;
					}
				}

				wgpu::SurfaceConfiguration config{};
				config.device          = g_Device;
				config.format          = g_SurfaceFormat;
				config.usage           = wgpu::TextureUsage::RenderAttachment;
				config.width           = width;
				config.height          = height;
				config.presentMode     = vs->PresentMode;
				config.alphaMode       = vs->AlphaMode;
				config.viewFormatCount = 0;

				vs->Surface.Configure(&config);
				vs->Width  = width;
				vs->Height = height;
				return true;
			}
		}

		// Re-Configure requires Unconfigure first on D3D12; called by SetVsync to propagate mode change to all viewport surfaces.
		void ReconfigureAllViewportSurfaces() {
			for (ViewportSurface* vs : g_LiveViewportSurfaces) {
				if (!vs || !vs->Surface || vs->Width == 0 || vs->Height == 0) continue;
				vs->CurrentView    = nullptr;
				vs->CurrentTexture = nullptr;
				vs->HasCurrent     = false;
				vs->Surface.Unconfigure();
				ConfigureViewportSurface(vs, vs->Width, vs->Height);
			}
		}

		ViewportSurface* CreateViewportSurface(void* hwnd, void* hinstance,
			uint32_t width, uint32_t height)
		{
			if (!g_Initialized || !g_Instance || !hwnd) return nullptr;

#if defined(IDX_PLATFORM_WINDOWS)
			wgpu::SurfaceSourceWindowsHWND hwndSource{};
			hwndSource.hwnd      = hwnd;
			hwndSource.hinstance = hinstance ? hinstance
				: static_cast<void*>(::GetModuleHandleW(nullptr));

			wgpu::SurfaceDescriptor surfaceDesc{};
			surfaceDesc.nextInChain = &hwndSource;

			wgpu::Surface surface = g_Instance.CreateSurface(&surfaceDesc);
			if (!surface) {
				IDX_CORE_ERROR_TAG("WebGPUApi",
					"CreateViewportSurface: instance.CreateSurface returned null");
				return nullptr;
			}

			auto* vs = new ViewportSurface{};
			vs->Surface = std::move(surface);
			if (!ConfigureViewportSurface(vs, width, height)) {
				IDX_CORE_ERROR_TAG("WebGPUApi",
					"CreateViewportSurface: ConfigureViewportSurface failed at {}x{}",
					width, height);
				delete vs;
				return nullptr;
			}
			g_LiveViewportSurfaces.push_back(vs);
			return vs;
#else
			(void)hwnd; (void)hinstance; (void)width; (void)height;
			IDX_CORE_ERROR_TAG("WebGPUApi",
				"CreateViewportSurface: only Windows is supported in Stage 1");
			return nullptr;
#endif
		}

		void DestroyViewportSurface(ViewportSurface* vs) {
			if (!vs) return;
			g_LiveViewportSurfaces.erase(
				std::remove(g_LiveViewportSurfaces.begin(), g_LiveViewportSurfaces.end(), vs),
				g_LiveViewportSurfaces.end());
			vs->CurrentView    = nullptr;
			vs->CurrentTexture = nullptr;
			if (vs->Surface) vs->Surface.Unconfigure();
			vs->Surface = nullptr;
			delete vs;
		}

		void ResizeViewportSurface(ViewportSurface* vs,
			uint32_t width, uint32_t height)
		{
			if (!vs || !vs->Surface || width == 0 || height == 0) return;
			if (vs->Width == width && vs->Height == height) return;
			vs->CurrentView    = nullptr;
			vs->CurrentTexture = nullptr;
			vs->HasCurrent     = false;
			ConfigureViewportSurface(vs, width, height);
		}

		wgpu::TextureView AcquireViewportSurfaceView(ViewportSurface* vs) {
			if (!vs || !vs->Surface) return nullptr;
			if (vs->HasCurrent) return vs->CurrentView;

			wgpu::SurfaceTexture st{};
			vs->Surface.GetCurrentTexture(&st);
			const bool ok =
				st.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
				st.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal;
			if (!ok) {
				return nullptr;
			}
			vs->CurrentTexture = wgpu::Texture(st.texture);

			wgpu::TextureViewDescriptor viewDesc{};
			viewDesc.format          = g_SurfaceFormat;
			viewDesc.dimension       = wgpu::TextureViewDimension::e2D;
			viewDesc.mipLevelCount   = 1;
			viewDesc.arrayLayerCount = 1;
			viewDesc.aspect          = wgpu::TextureAspect::All;
			vs->CurrentView = vs->CurrentTexture.CreateView(&viewDesc);
			vs->HasCurrent  = true;
			return vs->CurrentView;
		}

		void PresentViewportSurface(ViewportSurface* vs) {
			if (!vs || !vs->Surface || !vs->HasCurrent) return;
			vs->Surface.Present();
			vs->CurrentView    = nullptr;
			vs->CurrentTexture = nullptr;
			vs->HasCurrent     = false;
		}

		wgpu::TextureFormat GetViewportSurfaceFormat() { return g_SurfaceFormat; }

		wgpu::Instance GetInstance() { return g_Instance; }
	}

	// ── RenderApi: Lifecycle ────────────────────────────────────────────────

	bool RenderApi::Init(const GLInitSpecifications& spec) {
		if (g_Initialized) return false;

#if defined(IDX_PLATFORM_WINDOWS)
		// Win11 24H2: Dawn's LoadLibraryExA fails (error 87) on bare names without SetDefaultDllDirectories first; preloading short-circuits Dawn's own calls.
		const BOOL didSetDirs = ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		const HMODULE d3dCompiler = ::LoadLibraryW(L"d3dcompiler_47.dll");
		const HMODULE vulkan      = ::LoadLibraryW(L"vulkan-1.dll");
		IDX_CORE_INFO_TAG("WebGPUApi",
			"DLL preload: SetDefaultDllDirectories={} (last-error={}), d3dcompiler_47={}, vulkan-1={}",
			didSetDirs ? "ok" : "FAIL",
			didSetDirs ? 0u : static_cast<uint32_t>(::GetLastError()),
			d3dCompiler ? "ok" : "FAIL",
			vulkan ? "ok" : "missing (Vulkan SDK not installed; D3D will be used)");
#endif

		g_ClearColor = spec.ClearColor;
		g_VsyncEnabled = Window::IsVsync();

		Window* win = Application::GetWindow();
		if (win) {
			g_BackbufferWidth  = static_cast<uint32_t>(win->GetWidth());
			g_BackbufferHeight = static_cast<uint32_t>(win->GetHeight());
		}
		if (g_BackbufferWidth == 0)  g_BackbufferWidth  = 1280;
		if (g_BackbufferHeight == 0) g_BackbufferHeight = 720;

		// TimedWaitAny is REQUIRED: without it WaitAny rejects any non-zero timeout and adapter/device requests silently fail.
		const wgpu::InstanceFeatureName requiredFeatures[] = {
			wgpu::InstanceFeatureName::TimedWaitAny,
		};
		wgpu::InstanceDescriptor instanceDesc{};
		instanceDesc.requiredFeatureCount = 1;
		instanceDesc.requiredFeatures     = requiredFeatures;
		g_Instance = wgpu::CreateInstance(&instanceDesc);
		if (!g_Instance) {
			IDX_CORE_ERROR_TAG("WebGPUApi", "wgpu::CreateInstance failed");
			return false;
		}

		if (!CreateSurface())       { Shutdown(); return false; }
		if (!RequestAdapterSync())  { Shutdown(); return false; }
		if (!RequestDeviceSync())   { Shutdown(); return false; }
		ConfigureSurface(g_BackbufferWidth, g_BackbufferHeight);

		wgpu::AdapterInfo info{};
		g_Adapter.GetInfo(&info);
		g_VendorString   = FromStringView(info.vendor);
		g_RendererString = FromStringView(info.device);
		g_VersionString  = std::string("WebGPU (Dawn) / ") + BackendTypeName(info.backendType)
			+ " / " + AdapterTypeName(info.adapterType);

		IDX_CORE_INFO_TAG("WebGPUApi",
			"WebGPU initialized — adapter='{}' vendor='{}' backend={} type={} backbuffer={}x{} format={}",
			g_RendererString, g_VendorString,
			BackendTypeName(info.backendType), AdapterTypeName(info.adapterType),
			g_BackbufferWidth, g_BackbufferHeight,
			static_cast<int>(g_SurfaceFormat));

		g_Initialized = true;
		return true;
	}

	void RenderApi::Present() {
		WebGPUBackend::Present();
	}

	void RenderApi::Shutdown() {
		if (!g_Initialized && !g_Instance) return;

		g_Frame = FrameState{};
		if (g_Surface) g_Surface.Unconfigure();
		g_CurrentTarget = TargetState{};
		g_Queue   = nullptr;
		g_Device  = nullptr;
		g_Adapter = nullptr;
		g_Surface = nullptr;
		g_Instance = nullptr;

		g_VersionString.clear();
		g_VendorString.clear();
		g_RendererString.clear();
		g_SurfaceFormat = wgpu::TextureFormat::Undefined;

		g_Initialized = false;
	}

	bool RenderApi::IsInitialized() {
		return g_Initialized;
	}

	std::string_view RenderApi::BackendName() {
		return "webgpu";
	}

	const std::string& RenderApi::GetVersionString()  { return g_VersionString; }
	const std::string& RenderApi::GetVendorString()   { return g_VendorString; }
	const std::string& RenderApi::GetRendererString() { return g_RendererString; }

	// ── Per-frame state ─────────────────────────────────────────────────────

	void RenderApi::Clear(ClearFlags /*flags*/) {
		EnsureFrameEncoder();

		wgpu::TextureView targetColorView;
		if (g_CurrentTarget.IsSwapChain) {
			if (!EnsureSurfaceTexture()) return;
			targetColorView = g_Frame.SurfaceView;
		} else {
			targetColorView = g_CurrentTarget.ColorView;
			if (!targetColorView) {
				IDX_CORE_WARN_TAG("WebGPUApi", "Clear on FBO target with no colour view bound");
				return;
			}
		}

		EndActivePassIfAny();

		wgpu::RenderPassColorAttachment colorAtt{};
		colorAtt.view       = targetColorView;
		colorAtt.loadOp     = wgpu::LoadOp::Clear;
		colorAtt.storeOp    = wgpu::StoreOp::Store;
		colorAtt.clearValue = wgpu::Color{
			g_ClearColor.r, g_ClearColor.g, g_ClearColor.b, g_ClearColor.a };
		colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

		wgpu::RenderPassDescriptor passDesc{};
		passDesc.colorAttachmentCount = 1;
		passDesc.colorAttachments     = &colorAtt;

		wgpu::RenderPassDepthStencilAttachment depthAtt{};
		if (!g_CurrentTarget.IsSwapChain && g_CurrentTarget.DepthView) {
			depthAtt.view              = g_CurrentTarget.DepthView;
			depthAtt.depthLoadOp       = wgpu::LoadOp::Clear;
			depthAtt.depthStoreOp      = wgpu::StoreOp::Store;
			depthAtt.depthClearValue   = 1.0f;
			depthAtt.stencilLoadOp     = wgpu::LoadOp::Clear;
			depthAtt.stencilStoreOp    = wgpu::StoreOp::Store;
			depthAtt.stencilClearValue = 0;
			passDesc.depthStencilAttachment = &depthAtt;
		}

		wgpu::RenderPassEncoder pass = g_Frame.Encoder.BeginRenderPass(&passDesc);
		pass.End();

		if (g_CurrentTarget.IsSwapChain) {
			g_Frame.PresentedSwapChain = true;
		}
	}

	void RenderApi::SetClearColor(const Color& color) {
		g_ClearColor = color;
		// No GPU work — value picked up on next Clear / next render pass.
	}

	Color RenderApi::GetClearColor() {
		return g_ClearColor;
	}

	void RenderApi::SetViewport(int x, int y, int width, int height) {
		g_ViewportX = static_cast<uint32_t>(x < 0 ? 0 : x);
		g_ViewportY = static_cast<uint32_t>(y < 0 ? 0 : y);
		g_ViewportW = static_cast<uint32_t>(width  > 0 ? width  : 1);
		g_ViewportH = static_cast<uint32_t>(height > 0 ? height : 1);
	}

	void RenderApi::OnWindowResize(int width, int height) {
		if (!g_Initialized) return;
		if (width <= 0 || height <= 0) return;

		const uint32_t uw = static_cast<uint32_t>(width);
		const uint32_t uh = static_cast<uint32_t>(height);
		if (uw == g_BackbufferWidth && uh == g_BackbufferHeight) return;

#ifdef IDX_DEBUG
		IDX_CORE_INFO_TAG("WebGPUApi",
			"OnWindowResize: {}x{} -> {}x{} (surface.Configure)",
			g_BackbufferWidth, g_BackbufferHeight, uw, uh);
#endif

		g_Frame = FrameState{};
		// Unconfigure before Configure: D3D12 re-Configure reuses the swap chain and can preserve stale flags; clean teardown forces a fresh one.
		g_Surface.Unconfigure();
		ConfigureSurface(uw, uh);
	}

	void RenderApi::SetVsync(bool enabled) {
		g_VsyncEnabled = enabled;
		if (!g_Initialized || !g_Surface) return;
		if (g_BackbufferWidth == 0 || g_BackbufferHeight == 0) return;

		IDX_CORE_INFO_TAG("WebGPUApi",
			"SetVsync: enabled={} -> unconfigure + reconfigure", enabled);

		// g_Frame MUST reset before Unconfigure (Dawn rejects views that outlive the surface).
		g_Frame = FrameState{};
		g_Surface.Unconfigure();
		ConfigureSurface(g_BackbufferWidth, g_BackbufferHeight);
		WebGPUBackend::ReconfigureAllViewportSurfaces();
	}

	void RenderApi::SetScissor(int x, int y, int width, int height) {
		g_ScissorX = static_cast<uint32_t>(x < 0 ? 0 : x);
		g_ScissorY = static_cast<uint32_t>(y < 0 ? 0 : y);
		g_ScissorW = static_cast<uint32_t>(width  > 0 ? width  : 0);
		g_ScissorH = static_cast<uint32_t>(height > 0 ? height : 0);
		g_ScissorActive = (g_ScissorW > 0 && g_ScissorH > 0);
	}

	void RenderApi::EnableScissorTest()  { g_ScissorActive = (g_ScissorW > 0 && g_ScissorH > 0); }
	void RenderApi::DisableScissorTest() { g_ScissorActive = false; }

	void RenderApi::EnableDepthTest()                            { /* per-pipeline via DepthStencilState; Stage 2 */ }
	void RenderApi::DisableDepthTest()                           { /* per-pipeline via DepthStencilState; Stage 2 */ }
	void RenderApi::SetCullMode(CullMode /*mode*/)               { /* per-pipeline via PrimitiveState::cullMode; Stage 2 */ }
	void RenderApi::SetBlendMode(BlendMode /*mode*/)             { /* per-pipeline via BlendState; Stage 2 */ }
	void RenderApi::SetBlendingEnabled(bool /*enabled*/)         { /* per-pipeline via ColorTargetState::blend; Stage 2 */ }
	void RenderApi::SetPolygonMode(PolygonMode mode)             { g_PolygonMode = mode; }
	PolygonMode RenderApi::GetPolygonMode()                     { return g_PolygonMode; }
	void RenderApi::SetLineWidth(float /*width*/)                { /* WebGPU has no wide-lines feature — gpu-side wide lines or fat-line geometry shader in Stage 2 */ }
	void RenderApi::SetColorMask(bool /*r*/, bool /*g*/, bool /*b*/, bool /*a*/) {
		// Per-pipeline via ColorTargetState::writeMask; Stage 2.
	}

	void RenderApi::BindFramebuffer(const Framebuffer& fbo) {
		const uint32_t backendId = fbo.GetBackendId();
		if (backendId == 0) {
			BindDefaultFramebuffer();
			return;
		}

		const auto lookup = WebGPUBackend::LookupFramebufferByFboId(backendId);
		if (!lookup.Valid) {
			IDX_CORE_WARN_TAG("WebGPUApi",
				"BindFramebuffer: no GPU resources for fboId={} — falling back to swap chain",
				backendId);
			BindDefaultFramebuffer();
			return;
		}

		const bool sameTarget = !g_CurrentTarget.IsSwapChain
			&& g_CurrentTarget.ColorView.Get() == lookup.ColorView.Get();
		if (!sameTarget) {
			FlushFrameCommands();
		} else {
			EndActivePassIfAny();
		}

		g_CurrentTarget = TargetState{};
		g_CurrentTarget.ColorView   = lookup.ColorView;
		g_CurrentTarget.DepthView   = lookup.DepthView;
		g_CurrentTarget.ColorFormat = lookup.ColorFormat;
		g_CurrentTarget.IsSwapChain = false;
		g_CurrentTarget.Width       = lookup.Width;
		g_CurrentTarget.Height      = lookup.Height;
	}

	void RenderApi::BindDefaultFramebuffer() {
		if (!g_CurrentTarget.IsSwapChain) {
			FlushFrameCommands();
		} else {
			EndActivePassIfAny();
		}

		g_CurrentTarget = TargetState{};
		g_CurrentTarget.IsSwapChain = true;
		g_CurrentTarget.ColorFormat = g_SurfaceFormat;
		g_CurrentTarget.Width       = g_BackbufferWidth;
		g_CurrentTarget.Height      = g_BackbufferHeight;
	}

}  // namespace Index
