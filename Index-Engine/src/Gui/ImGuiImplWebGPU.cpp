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
    // Needed for the per-viewport HWND adjustments below: owner-relationship
    // wiring (SetWindowLongPtrW + GWLP_HWNDPARENT) and DWM titlebar-theme
    // override (DwmSetWindowAttribute + DWMWA_USE_IMMERSIVE_DARK_MODE).
    // dwmapi.lib is already linked into engine.dll by Win32Titlebar.cpp.
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

// =============================================================================
// imgui_impl_wgpu wrapper.
// -----------------------------------------------------------------------------
// Thin Index shim around Dear ImGui's official imgui_impl_wgpu backend (in
// External/imgui/backends/).
//
// Why a wrapper rather than calling imgui_impl_wgpu directly from the
// editor / launcher / runtime:
//   1. wgpu::Device + Queue + per-frame command encoder live in engine.dll
//      (WebGPUApi.cpp). The backend has to call into those, so it must run
//      in the same module.
//   2. PackageImGuiBridge synchronises the consumer's ImGuiContext into
//      engine.dll's ImGui copy on every entry. Without this, the engine
//      DLL would see a null GImGui pointer and crash inside ImGui::GetIO.
//   3. The ABI-stable INDEX_API surface (`Init`, `NewFrame`,
//      `RenderDrawData(d, viewId)`, `Shutdown`) keeps editor/launcher/
//      runtime call sites backend-neutral.
//
// imgui_impl_wgpu uses Dawn's C API (`WGPUDevice`, `WGPURenderPassEncoder`).
// We pass wgpu::Device through .Get() to drop into the C ABI; equivalent
// for the pass encoder when we hand it to ImGui_ImplWGPU_RenderDrawData.
// =============================================================================

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
		// Apply our chrome conventions to a viewport HWND:
		//   * Strip WS_EX_TOOLWINDOW (which imgui_impl_glfw applies when
		//     the viewport has NoTaskBarIcon — gives the chunky red close
		//     button. We use owner-window relationship to hide from the
		//     taskbar instead).
		//   * Pick chrome based on the viewport's NoAutoMerge flag:
		//       dialog (helper-marked):  WS_CAPTION + WS_SYSMENU + WS_THICKFRAME
		//                                (just close, no min/max — matches a
		//                                short modal form)
		//       panel  (undocked window): WS_CAPTION + WS_SYSMENU + WS_THICKFRAME
		//                                + WS_MINIMIZEBOX + WS_MAXIMIZEBOX
		//                                (full app chrome — undocked panels
		//                                are meant to act like standalone
		//                                tool windows)
		//   * SWP_FRAMECHANGED so Windows re-evaluates the non-client area.
		//   * DWM immersive dark mode so the titlebar matches the launcher/
		//     editor's dark theme.
		//
		// Called from both MultiViewport_CreateWindow (initial chrome) and
		// MultiViewport_ShowWindow (re-applied each show — imgui_impl_glfw's
		// Platform_ShowWindow re-adds WS_EX_TOOLWINDOW and DWM resets the
		// immersive-dark-mode attribute on first visibility on some builds).
		void ApplyViewportChrome(HWND hwnd, ImGuiViewport* viewport) {
			if (!hwnd) return;

			const bool isDialog =
				viewport != nullptr &&
				(viewport->Flags & ImGuiViewportFlags_NoAutoMerge) != 0;

			LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
			exStyle &= ~WS_EX_TOOLWINDOW;
			SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

			LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
			if (isDialog) {
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

			BOOL useDarkMode = TRUE;
			DwmSetWindowAttribute(
				hwnd,
				DWMWA_USE_IMMERSIVE_DARK_MODE,
				&useDarkMode,
				sizeof(useDarkMode));
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

		// Render target format = swap-chain format. The editor draws ImGui
		// directly onto the OS window (the per-FBO scene panels are ImGui
		// images, not ImGui draw lists). One pipeline per session is
		// enough — Dawn caches it internally.
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

		// Translate any pending OS-level close requests (X-button clicks on
		// secondary viewports → WM_CLOSE → glfwSetWindowCloseCallback →
		// viewport->PlatformRequestClose = true) into ImGui popup closes.
		// ImGui only auto-handles PlatformRequestClose for top-level Begin()
		// windows with a `&p_open` pointer — popups stay on the popup stack
		// regardless, so without this step clicking the X on a
		// BeginPopupModal viewport is a no-op (the X visibly depresses but
		// the modal stays open). Walk the open popup stack and, for each
		// popup whose window lives in a viewport with PlatformRequestClose
		// set, close that popup level (and everything stacked on top of it).
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
		// Resync engine.dll's ImGui context from the consumer's bridge so
		// every helper inside ImGui_ImplWGPU_RenderDrawData (notably
		// ImGui_ImplWGPU_CreateImageBindGroup, which re-fetches the
		// backend data via ImGui::GetIO() each time it's called) sees
		// the right ImGuiContext. Without this, the inner re-fetch can
		// see a stale/null GImGui inside engine.dll and dereference a
		// null `bd` -> 0xC0000005 access violation reading at
		// `bd->wgpuDevice` offset.
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

		// Resolve the current target. Editor's ImGui pass typically runs
		// last in the frame on the swap chain (after Renderer2D / Gui-
		// Renderer / Text / Gizmo passes). Whatever the most recent
		// BindFramebuffer set is what we draw onto.
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
		// We initialised ImGui_ImplWGPU with DepthStencilFormat=Undefined,
		// so the pipeline doesn't expect a depth attachment. Always omit
		// it from the pass descriptor even if the target has one — the
		// underlying texture stays untouched.
		(void)depthAtt;

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

	// ── Multi-viewport renderer callbacks ───────────────────────────────
	// Ground truth on the data layout: imgui_impl_glfw stores the GLFWwindow*
	// in viewport->PlatformHandle and the HWND in viewport->PlatformHandleRaw
	// on Win32 (set inside its Renderer_CreateWindow). We don't need to
	// reach back into glfw3native.h here — the HWND is already exposed for us.

	namespace {

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

			// (1) Owner-window relationship: tie the popup's z-order to the
			// main app HWND. Effects:
			//   * The popup is always rendered ABOVE its owner in z-order
			//     (the user can't accidentally bury a dialog by clicking
			//     the launcher/editor behind it),
			//   * But NOT above unrelated apps (an Alt-Tab to a browser
			//     still puts the popup behind the browser — the right
			//     behaviour for a dialog),
			//   * Minimising the owner hides the popup too; closing the
			//     owner destroys it.
			//   * Windows also excludes owned windows from the taskbar by
			//     default — so this alone is what hides the popup from the
			//     taskbar; we don't need WS_EX_TOOLWINDOW for that, and
			//     in fact strip it below.
			//
			// GWLP_HWNDPARENT is misnamed in the Win32 API — it sets the
			// *owner*, not the parent. Allowed at any time on a top-level
			// HWND.
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

			// (2) Apply our standard viewport chrome (strip WS_EX_TOOLWINDOW,
			// pick dialog-vs-panel style based on NoAutoMerge, force dark
			// titlebar). ApplyViewportChrome is shared with the ShowWindow
			// hook — see its definition above for the full rationale. Note
			// that Platform_ShowWindow runs AFTER us and would re-add
			// WS_EX_TOOLWINDOW; the ShowWindow hook re-applies this same
			// chrome to win that race.
			ApplyViewportChrome(viewportHwnd, viewport);

			// (2b) Inherit the owner's icon so the popup's titlebar shows
			// the Index app icon next to the title (matching the launcher's
			// chrome). Owned windows don't auto-pick up their owner's icon
			// in Win32 — you have to push it explicitly via WM_SETICON.
			// Fall back to the window-class icon if WM_GETICON returns null
			// (the executable manifest puts the icon on the class, not
			// directly on the HWND, on some Windows builds).
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

			// (3) DWM dark titlebar — match the launcher/editor's chrome.
			// Both main apps run with DWM immersive dark mode on (see
			// Win32Titlebar.cpp / SetTitlebarDarkMode), so popup titlebars
			// should be dark too. Hard-coded TRUE for now; if/when the
			// engine grows a runtime light-theme switch, this should read
			// the same flag the main window uses.
			BOOL useDarkMode = TRUE;
			DwmSetWindowAttribute(
				viewportHwnd,
				DWMWA_USE_IMMERSIVE_DARK_MODE,
				&useDarkMode,
				sizeof(useDarkMode));
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

			// Per-viewport encoder + submit. Atomic w.r.t. ImGui_ImplWGPU's
			// internal queue.WriteBuffer uploads: Dawn flushes pending writes
			// at Submit time, so vertex/index/uniform copies queued by THIS
			// RenderDrawData call land in the buffers immediately before
			// THIS pass executes — even though every viewport shares the
			// same FrameResources slot (indexed by ImGui_ImplWGPU's internal
			// frameIndex, which advances once per NewFrame, not per viewport).
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

		// Wrapper around imgui_impl_glfw's Platform_ShowWindow that fixes
		// two things the upstream backend gets wrong for our use case:
		//
		//   1. Chrome regression. imgui_impl_glfw's Platform_ShowWindow
		//      forcibly sets WS_EX_TOOLWINDOW when the viewport has
		//      ImGuiViewportFlags_NoTaskBarIcon. Tool-window chrome strips
		//      WS_THICKFRAME and triggers the chunky "compact dialog"
		//      close button (the red X). MultiViewport_CreateWindow stripped
		//      WS_EX_TOOLWINDOW already, but Platform_ShowWindow runs AFTER
		//      Renderer_CreateWindow and re-adds it. We re-strip it here.
		//      Owner relationship (set in MultiViewport_CreateWindow) keeps
		//      the popup out of the taskbar without needing WS_EX_TOOLWINDOW.
		//
		//   2. No auto-focus. GLFW creates viewport windows with
		//      GLFW_FOCUSED=false / GLFW_FOCUS_ON_SHOW=false so they don't
		//      steal focus during routine dock/undock. For modal dialogs
		//      opened by an explicit button click this is the wrong default:
		//      the user expects to start typing into the dialog immediately.
		//      SetForegroundWindow is allowed here because the recent user-
		//      input click that triggered the open grants us the foreground-
		//      steal permission Windows requires.
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

			// Pull the popup forward and give it keyboard focus. Both calls
			// matter: SetForegroundWindow handles z-order + taskbar focus
			// notification; SetFocus routes keystrokes to the new HWND.
			// The recent-user-input click that triggered the open grants
			// us the foreground-steal permission Windows requires.
			SetForegroundWindow(hwnd);
			SetFocus(hwnd);
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

		// Wrap the platform-side Show handler. Must run AFTER
		// ImGui_ImplGlfw_InitForOther (which sets Platform_ShowWindow on
		// platform_io). RegisterMultiViewportHandlers is called from the
		// consumer's OnAttach right after Init, so the ordering is correct.
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
		// Clear everything ImGui's popup logic hardcodes that would damage
		// our chrome:
		//   NoDecoration → native OS titlebar instead of ImGui's drawn one
		//   TopMost      → keep WS_THICKFRAME (resizable, normal close button)
		//   NoTaskBarIcon → don't trip imgui_impl_glfw into re-applying
		//                   WS_EX_TOOLWINDOW. Owner relationship hides the
		//                   window from the taskbar without needing it.
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

		// Flush the engine's per-frame encoder so the main window's
		// queued WriteBuffer uploads land before viewport renders
		// queue their own (shared FrameResources slot — see header
		// comment). Surface texture stays alive for Window::SwapBuffers
		// to present later in the frame.
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
