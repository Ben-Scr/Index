#include "pch.hpp"
#include "Gui/ImGuiImplWebGPU.hpp"

#include "Core/Application.hpp"
#include "Core/Log.hpp"
#include "Core/Window.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Packages/PackageImGuiBridge.hpp"

#include <webgpu/webgpu.h>
#include <webgpu/webgpu_cpp.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_wgpu.h>

#if defined(IDX_PLATFORM_WINDOWS)
    // Needed for HWND owner-wiring and DWM dark-mode; dwmapi.lib linked via Win32Titlebar.cpp.
    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3.h>
    #include <GLFW/glfw3native.h>
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <Windows.h>
    #include <dwmapi.h>
    #pragma comment(lib, "dwmapi.lib")
#endif

// Thin shim around imgui_impl_wgpu. Must live in engine.dll: WebGPU device/queue live here, and
// PackageImGuiBridge must resync the consumer's ImGuiContext on every entry (null GImGui → crash).

namespace Index::ImGuiImplWebGPU {

	namespace {
		bool g_Initialized = false;
		bool g_MultiViewportRegistered = false;
		// Bridge generation tracking — resyncs after the consumer
		// hot-reloads its ImGuiContext.
		unsigned long long g_LastSyncedGen = 0;

		// imgui_impl_glfw's original Platform_ShowWindow, saved so our
		// hook can chain to it. See RegisterMultiViewportHandlers and
		// MultiViewport_ShowWindow below for why we wrap it.
		void (*g_OriginalPlatformShowWindow)(ImGuiViewport*) = nullptr;

#if defined(IDX_PLATFORM_WINDOWS)
		// Strip WS_EX_TOOLWINDOW + WS_EX_APPWINDOW (APPWINDOW overrides owner-hiding), apply popup/dialog/panel chrome per flags, dark titlebar.
		// Re-applied in ShowWindow: imgui_impl_glfw's Platform_ShowWindow re-adds WS_EX_TOOLWINDOW and DWM resets dark-mode on first visibility.
		void ApplyViewportChrome(HWND hwnd, ImGuiViewport* viewport) {
			if (!hwnd) return;

			const bool isDialog =
				viewport != nullptr &&
				(viewport->Flags & ImGuiViewportFlags_NoAutoMerge) != 0;
			// NoDecoration = transient popup (tooltip, combo, menu) — must not get a titlebar or thick frame.
			const bool noDecoration =
				viewport != nullptr &&
				(viewport->Flags & ImGuiViewportFlags_NoDecoration) != 0;

			LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
			exStyle &= ~WS_EX_TOOLWINDOW;
			exStyle &= ~WS_EX_APPWINDOW;
			SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

			LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
			if (noDecoration) {
				style &= ~(WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
					WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME);
				style |= WS_POPUP;
			} else if (isDialog) {
				style |= WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
				style &= ~(WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
			} else {
				style |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
					WS_MAXIMIZEBOX | WS_THICKFRAME;
			}
			SetWindowLongPtrW(hwnd, GWL_STYLE, style);

			SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
				SWP_FRAMECHANGED | SWP_NOSIZE | SWP_NOMOVE |
				SWP_NOZORDER | SWP_NOACTIVATE);

			// Immersive dark mode only matters for chromed windows — popups
			// have no titlebar to tint. Skipping the DwmSetWindowAttribute
			// call also avoids a one-frame visual hiccup on transient popups.
			if (!noDecoration) {
				BOOL useDarkMode = TRUE;
				DwmSetWindowAttribute(
					hwnd,
					DWMWA_USE_IMMERSIVE_DARK_MODE,
					&useDarkMode,
					sizeof(useDarkMode));
			}
		}
#endif

		void SyncImGuiContextFromBridge() {
			const unsigned long long gen = PackageImGuiBridge::GetGeneration();
			if (gen == g_LastSyncedGen && ImGui::GetCurrentContext() != nullptr) {
				return;
			}
			void* ctx = PackageImGuiBridge::GetContext();
			if (ctx == nullptr) {
				return;
			}
			ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));
			void* allocFn = nullptr;
			void* freeFn  = nullptr;
			void* user    = nullptr;
			PackageImGuiBridge::GetAllocators(allocFn, freeFn, user);
			if (allocFn != nullptr && freeFn != nullptr) {
				ImGui::SetAllocatorFunctions(
					reinterpret_cast<ImGuiMemAllocFunc>(allocFn),
					reinterpret_cast<ImGuiMemFreeFunc>(freeFn),
					user);
			}
			g_LastSyncedGen = gen;
		}
	}

	bool Init() {
		if (g_Initialized) return true;

		SyncImGuiContextFromBridge();
		if (ImGui::GetCurrentContext() == nullptr) {
			// Consumer hasn't published a context yet — try again on the
			// next entry.
			return false;
		}

		if (!WebGPUBackend::IsInitialized()) {
			IDX_CORE_ERROR_TAG("ImGuiImplWebGPU",
				"Init called before WebGPU backend initialized");
			return false;
		}

		wgpu::Device device = WebGPUBackend::GetDevice();
		if (!device) {
			IDX_CORE_ERROR_TAG("ImGuiImplWebGPU", "No wgpu::Device available");
			return false;
		}

		ImGui_ImplWGPU_InitInfo info{};
		info.Device                          = device.Get();
		info.NumFramesInFlight               = 3;
		info.RenderTargetFormat              = static_cast<WGPUTextureFormat>(
			WebGPUBackend::GetSurfaceFormat());
		info.DepthStencilFormat              = WGPUTextureFormat_Undefined;
		info.PipelineMultisampleState        = {};
		info.PipelineMultisampleState.count  = 1;
		info.PipelineMultisampleState.mask   = 0xFFFFFFFFu;

		if (!ImGui_ImplWGPU_Init(&info)) {
			IDX_CORE_ERROR_TAG("ImGuiImplWebGPU", "ImGui_ImplWGPU_Init failed");
			return false;
		}

		g_Initialized = true;
		IDX_CORE_INFO_TAG("ImGuiImplWebGPU",
			"ImGui WebGPU backend initialized (target format={})",
			static_cast<int>(info.RenderTargetFormat));
		return true;
	}

	void NewFrame() {
		if (!g_Initialized) return;
		// Resync to pick up consumer-side context hot-reloads before any
		// ImGui call.
		SyncImGuiContextFromBridge();

		// ImGui only auto-handles PlatformRequestClose for top-level Begin() windows; popup stack entries need manual close or clicking X on a BeginPopupModal is a no-op.
		if (ImGui::GetCurrentContext() != nullptr) {
			ImGuiContext& g = *ImGui::GetCurrentContext();
			for (int i = g.OpenPopupStack.Size - 1; i >= 0; --i) {
				ImGuiWindow* w = g.OpenPopupStack[i].Window;
				if (!w || !w->Viewport) continue;
				if (!w->Viewport->PlatformRequestClose) continue;
				// Consume the request before closing so we don't re-trigger
				// next frame if ImGui's close path doesn't clear it itself.
				w->Viewport->PlatformRequestClose = false;
				ImGui::ClosePopupToLevel(i, /*restore_focus_to_window_under_popup*/ true);
				break;
			}
		}

		ImGui_ImplWGPU_NewFrame();
	}

	void RenderDrawData(ImDrawData* drawData, unsigned short /*viewId*/) {
		if (!g_Initialized || drawData == nullptr || drawData->CmdListsCount <= 0) {
			return;
		}
		// Resync required: ImGui_ImplWGPU_RenderDrawData re-fetches backend data via GetIO() internally; stale GImGui in engine.dll → null `bd` → crash at bd->wgpuDevice.
		SyncImGuiContextFromBridge();
		if (ImGui::GetCurrentContext() == nullptr) return;
		if (ImGui::GetIO().BackendRendererUserData == nullptr) {
			IDX_CORE_WARN_TAG("ImGuiImplWebGPU",
				"RenderDrawData: BackendRendererUserData is null after sync — "
				"editor's ImGui context likely didn't have ImGui_ImplWGPU_Init "
				"called on it. Skipping draw.");
			return;
		}

		const float fbW = drawData->DisplaySize.x * drawData->FramebufferScale.x;
		const float fbH = drawData->DisplaySize.y * drawData->FramebufferScale.y;
		if (fbW <= 0.0f || fbH <= 0.0f) return;

		auto target = WebGPUBackend::BeginRenderToCurrentTarget();
		if (!target.Valid) return;

		wgpu::CommandEncoder encoder = WebGPUBackend::GetFrameEncoder();
		if (!encoder) return;

		wgpu::RenderPassColorAttachment colorAtt{};
		colorAtt.view       = target.ColorView;
		colorAtt.loadOp     = wgpu::LoadOp::Load;
		colorAtt.storeOp    = wgpu::StoreOp::Store;
		colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

		wgpu::RenderPassDepthStencilAttachment depthAtt{};
		(void)depthAtt; // DepthStencilFormat=Undefined: pipeline expects no depth attachment even if the target has one.

		wgpu::RenderPassDescriptor passDesc{};
		passDesc.label                  = "imgui-pass";
		passDesc.colorAttachmentCount   = 1;
		passDesc.colorAttachments       = &colorAtt;
		passDesc.depthStencilAttachment = nullptr;

		wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);

		// Hand the pass off to ImGui's official backend. It records its
		// SetPipeline / SetVertexBuffer / SetIndexBuffer / SetScissorRect
		// / Draw calls into our encoder.
		ImGui_ImplWGPU_RenderDrawData(drawData, pass.Get());

		pass.End();

		if (target.IsSwapChain) {
			WebGPUBackend::MarkSwapChainRendered();
		}
	}

	void Shutdown() {
		if (!g_Initialized) return;
		UnregisterMultiViewportHandlers();
		ImGui_ImplWGPU_Shutdown();
		g_Initialized = false;
	}

	// imgui_impl_glfw stores GLFWwindow* in PlatformHandle and HWND in PlatformHandleRaw on Win32.

	namespace {

		// Forward declaration so MultiViewport_CreateWindow below can wire
		// up the refresh callback (defined further down).
#if defined(IDX_PLATFORM_WINDOWS)
		void MultiViewport_RefreshCallback(GLFWwindow* glfwWindow);
#endif

		void MultiViewport_CreateWindow(ImGuiViewport* viewport) {
			void* hwnd = viewport->PlatformHandleRaw;
			if (!hwnd) {
				IDX_CORE_ERROR_TAG("ImGuiImplWebGPU",
					"Renderer_CreateWindow: viewport has no PlatformHandleRaw (HWND); "
					"imgui_impl_glfw didn't populate it. Skipping.");
				viewport->RendererUserData = nullptr;
				return;
			}

			const uint32_t w = static_cast<uint32_t>(viewport->Size.x);
			const uint32_t h = static_cast<uint32_t>(viewport->Size.y);

			auto* vs = WebGPUBackend::CreateViewportSurface(
				hwnd, /*hinstance*/ nullptr,
				w > 0 ? w : 1, h > 0 ? h : 1);
			viewport->RendererUserData = vs;

#if defined(IDX_PLATFORM_WINDOWS)
			HWND viewportHwnd = static_cast<HWND>(hwnd);

			// Set GWLP_HWNDPARENT (misnamed — sets *owner*, not parent) to tie z-order and taskbar hiding to the main HWND without needing WS_EX_TOOLWINDOW.
			HWND ownerHwnd = nullptr;
			Window* mainWindow = Application::GetWindow();
			GLFWwindow* mainGlfw = mainWindow ? mainWindow->GetGLFWWindow() : nullptr;
			if (mainGlfw) {
				ownerHwnd = glfwGetWin32Window(mainGlfw);
				if (ownerHwnd) {
					SetWindowLongPtrW(
						viewportHwnd,
						GWLP_HWNDPARENT,
						reinterpret_cast<LONG_PTR>(ownerHwnd));
				}
			}

			ApplyViewportChrome(viewportHwnd, viewport);

			// Owned windows don't inherit the owner's icon — push via WM_SETICON; fall back to the window-class icon (manifest puts it there on some builds).
			if (ownerHwnd) {
				HICON smallIcon = reinterpret_cast<HICON>(
					SendMessageW(ownerHwnd, WM_GETICON, ICON_SMALL, 0));
				if (!smallIcon) {
					smallIcon = reinterpret_cast<HICON>(
						GetClassLongPtrW(ownerHwnd, GCLP_HICONSM));
				}
				if (smallIcon) {
					SendMessageW(viewportHwnd, WM_SETICON, ICON_SMALL,
						reinterpret_cast<LPARAM>(smallIcon));
				}

				HICON bigIcon = reinterpret_cast<HICON>(
					SendMessageW(ownerHwnd, WM_GETICON, ICON_BIG, 0));
				if (!bigIcon) {
					bigIcon = reinterpret_cast<HICON>(
						GetClassLongPtrW(ownerHwnd, GCLP_HICON));
				}
				if (bigIcon) {
					SendMessageW(viewportHwnd, WM_SETICON, ICON_BIG,
						reinterpret_cast<LPARAM>(bigIcon));
				}
			}

			BOOL useDarkMode = TRUE;
			DwmSetWindowAttribute(
				viewportHwnd,
				DWMWA_USE_IMMERSIVE_DARK_MODE,
				&useDarkMode,
				sizeof(useDarkMode));

			// Refresh callback re-renders during Win32's modal sizing loop (main thread blocked in DefWindowProc); prevents the "stretched framebuffer" artifact on resize.
			GLFWwindow* viewportGlfw =
				static_cast<GLFWwindow*>(viewport->PlatformHandle);
			if (viewportGlfw) {
				glfwSetWindowRefreshCallback(viewportGlfw,
					MultiViewport_RefreshCallback);
			}
#endif
		}

		void MultiViewport_DestroyWindow(ImGuiViewport* viewport) {
			auto* vs = static_cast<WebGPUBackend::ViewportSurface*>(
				viewport->RendererUserData);
			WebGPUBackend::DestroyViewportSurface(vs);
			viewport->RendererUserData = nullptr;
		}

		void MultiViewport_SetWindowSize(ImGuiViewport* viewport, ImVec2 size) {
			auto* vs = static_cast<WebGPUBackend::ViewportSurface*>(
				viewport->RendererUserData);
			if (!vs) return;
			const uint32_t w = static_cast<uint32_t>(size.x);
			const uint32_t h = static_cast<uint32_t>(size.y);
			if (w == 0 || h == 0) return;
			WebGPUBackend::ResizeViewportSurface(vs, w, h);
		}

		void MultiViewport_RenderWindow(ImGuiViewport* viewport, void* /*render_arg*/) {
			auto* vs = static_cast<WebGPUBackend::ViewportSurface*>(
				viewport->RendererUserData);
			if (!vs) return;

			// Same context-sync dance as the main RenderDrawData — ImGui state
			// lives in the consumer (launcher/editor) while ImGui_ImplWGPU's
			// backend data lives in engine.dll's ImGui copy.
			SyncImGuiContextFromBridge();
			if (ImGui::GetCurrentContext() == nullptr) return;
			if (ImGui::GetIO().BackendRendererUserData == nullptr) return;

			wgpu::TextureView view = WebGPUBackend::AcquireViewportSurfaceView(vs);
			if (!view) return;

			wgpu::Device device = WebGPUBackend::GetDevice();
			if (!device) return;

			wgpu::CommandEncoderDescriptor encDesc{};
			encDesc.label = "imgui-viewport-encoder";
			wgpu::CommandEncoder encoder = device.CreateCommandEncoder(&encDesc);

			wgpu::RenderPassColorAttachment colorAtt{};
			colorAtt.view       = view;
			colorAtt.loadOp     = (viewport->Flags & ImGuiViewportFlags_NoRendererClear)
				? wgpu::LoadOp::Load
				: wgpu::LoadOp::Clear;
			colorAtt.storeOp    = wgpu::StoreOp::Store;
			colorAtt.clearValue = wgpu::Color{ 0.0, 0.0, 0.0, 0.0 };
			colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

			wgpu::RenderPassDescriptor passDesc{};
			passDesc.label                  = "imgui-viewport-pass";
			passDesc.colorAttachmentCount   = 1;
			passDesc.colorAttachments       = &colorAtt;
			passDesc.depthStencilAttachment = nullptr;

			wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
			ImGui_ImplWGPU_RenderDrawData(viewport->DrawData, pass.Get());
			pass.End();

			wgpu::CommandBufferDescriptor cbDesc{};
			wgpu::CommandBuffer cmd = encoder.Finish(&cbDesc);

			wgpu::Queue queue = WebGPUBackend::GetQueue();
			queue.Submit(1, &cmd);
		}

		void MultiViewport_SwapBuffers(ImGuiViewport* viewport, void* /*render_arg*/) {
			auto* vs = static_cast<WebGPUBackend::ViewportSurface*>(
				viewport->RendererUserData);
			WebGPUBackend::PresentViewportSurface(vs);
		}

#if defined(IDX_PLATFORM_WINDOWS)
		// Fires on WM_PAINT during Win32's modal sizing loop (main thread blocked in DefWindowProc); runs a full re-layout pass so the viewport isn't just stretched stale framebuffer.
		void MultiViewport_RefreshCallback(GLFWwindow* /*glfwWindow*/) {
			Application* app = Application::GetInstance();
			if (!app) return;
			app->RenderOnceForRefresh();
		}
#endif

		// Hooks Platform_ShowWindow: re-strips WS_EX_TOOLWINDOW (upstream re-adds it for NoTaskBarIcon) and gives keyboard focus to dialogs (GLFW defaults to FOCUSED=false).
		void MultiViewport_ShowWindow(ImGuiViewport* viewport) {
			// Chain to imgui_impl_glfw first — it actually shows the GLFW
			// window (and re-applies WS_EX_TOOLWINDOW we're about to undo).
			if (g_OriginalPlatformShowWindow) {
				g_OriginalPlatformShowWindow(viewport);
			}

#if defined(IDX_PLATFORM_WINDOWS)
			HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
			if (!hwnd) return;

			// Re-apply our chrome after imgui_impl_glfw's Platform_ShowWindow
			// (it re-added WS_EX_TOOLWINDOW for NoTaskBarIcon viewports, and
			// on some Windows builds DWM resets the dark-mode attribute when
			// the window first becomes visible).
			ApplyViewportChrome(hwnd, viewport);

			const bool noFocusOnAppearing =
				(viewport->Flags & ImGuiViewportFlags_NoFocusOnAppearing) != 0;
			if (!noFocusOnAppearing) {
				SetForegroundWindow(hwnd);
				SetFocus(hwnd);
			}
#endif
		}

	}  // namespace

	void RegisterMultiViewportHandlers() {
		if (g_MultiViewportRegistered) return;
		SyncImGuiContextFromBridge();
		if (ImGui::GetCurrentContext() == nullptr) {
			IDX_CORE_WARN_TAG("ImGuiImplWebGPU",
				"RegisterMultiViewportHandlers: no ImGui context published yet "
				"(register after the consumer publishes via PackageImGuiBridge).");
			return;
		}
		ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
		pio.Renderer_CreateWindow  = MultiViewport_CreateWindow;
		pio.Renderer_DestroyWindow = MultiViewport_DestroyWindow;
		pio.Renderer_SetWindowSize = MultiViewport_SetWindowSize;
		pio.Renderer_RenderWindow  = MultiViewport_RenderWindow;
		pio.Renderer_SwapBuffers   = MultiViewport_SwapBuffers;

		// Must run AFTER ImGui_ImplGlfw_InitForOther, which sets Platform_ShowWindow; consumer calls this from OnAttach right after Init so the ordering is guaranteed.
		g_OriginalPlatformShowWindow = pio.Platform_ShowWindow;
		pio.Platform_ShowWindow = MultiViewport_ShowWindow;

		g_MultiViewportRegistered = true;
		IDX_CORE_INFO_TAG("ImGuiImplWebGPU",
			"Multi-viewport renderer handlers registered (with Platform_ShowWindow hook).");
	}

	void SetNextWindowAsNativeDialog() {
		// Sync first so the ImGuiWindowClass we attach lands on the
		// consumer's ImGuiContext, not engine.dll's local one.
		SyncImGuiContextFromBridge();
		if (ImGui::GetCurrentContext() == nullptr) return;

		ImGuiWindowClass wc{};
		// NoAutoMerge — always its own OS window.
		wc.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
		// Clear these or ImGui popup logic re-applies them: NoDecoration strips titlebar; TopMost strips WS_THICKFRAME; NoTaskBarIcon trips imgui_impl_glfw into re-adding WS_EX_TOOLWINDOW.
		wc.ViewportFlagsOverrideClear =
			ImGuiViewportFlags_NoDecoration |
			ImGuiViewportFlags_TopMost |
			ImGuiViewportFlags_NoTaskBarIcon;
		ImGui::SetNextWindowClass(&wc);
	}

	void RenderPlatformWindowsDefault() {
		if (!g_Initialized) return;
		SyncImGuiContextFromBridge();
		if (ImGui::GetCurrentContext() == nullptr) return;

		ImGuiIO& io = ImGui::GetIO();
		if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0) {
			return;
		}

		// Flush before viewport renders queue writes into the same shared FrameResources slot.
		WebGPUBackend::FlushCommands();

		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
	}

	void UnregisterMultiViewportHandlers() {
		if (!g_MultiViewportRegistered) return;
		// ImGui drains its secondary viewports (calling Renderer_DestroyWindow
		// on each) during ImGui::DestroyContext, so by the time anyone calls
		// Shutdown() we shouldn't have surviving ViewportSurfaces. Just
		// null the function pointers so any stray PlatformIO::WindowsList
		// traversal during teardown becomes a clean no-op.
		ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
		pio.Renderer_CreateWindow  = nullptr;
		pio.Renderer_DestroyWindow = nullptr;
		pio.Renderer_SetWindowSize = nullptr;
		pio.Renderer_RenderWindow  = nullptr;
		pio.Renderer_SwapBuffers   = nullptr;

		// Restore imgui_impl_glfw's original Platform_ShowWindow so its
		// Shutdown can run normally (it doesn't expect our wrapper to be
		// in place).
		if (g_OriginalPlatformShowWindow) {
			pio.Platform_ShowWindow = g_OriginalPlatformShowWindow;
			g_OriginalPlatformShowWindow = nullptr;
		}

		g_MultiViewportRegistered = false;
	}

}  // namespace Index::ImGuiImplWebGPU
