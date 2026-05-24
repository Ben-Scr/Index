#pragma once

#include "Core/Export.hpp"

// ImGui backend that submits draw lists through the Dawn-backed WebGPU
// device. Wraps Dear ImGui's official `imgui_impl_wgpu` backend (vendored
// under External/imgui/backends/) with an `Init / NewFrame /
// RenderDrawData / Shutdown` surface.
//
// IMPORTANT: this code lives in Index-Engine.dll. wgpu::Device is owned
// by engine.dll's WebGPUApi.cpp, so the backend that consumes it has to
// live in the same module. PackageImGuiBridge syncs the consumer's
// ImGuiContext into engine.dll's ImGui copy on every entry point.

struct ImDrawData;

namespace Index::ImGuiImplWebGPU {

	// Initialise the Dear ImGui WebGPU backend. Runs after ImGui::Create-
	// Context and imgui_impl_glfw::Init. Picks up the active wgpu::Device
	// + surface format from WebGPUApi.cpp's internal state. Idempotent;
	// returns true once successful and on subsequent calls. Returns false
	// if the backend isn't initialised yet (device not ready).
	INDEX_API bool Init();

	// Per-frame setup. Forwards to ImGui_ImplWGPU_NewFrame, which inspects
	// the font atlas for changes and re-uploads if needed. Cheap when
	// nothing changed.
	INDEX_API void NewFrame();

	// Submit ImDrawData. The `viewId` parameter is preserved for ABI
	// stability but is ignored under WebGPU — the active render target is
	// determined by the most recent RenderApi::BindFramebuffer (typically
	// the swap chain for the editor's ImGui pass).
	//
	// Internally: opens a wgpu::RenderPassEncoder with LoadOp::Load on the
	// current target, dispatches to ImGui_ImplWGPU_RenderDrawData with the
	// pass encoder, then ends the pass. Marks the swap chain as rendered
	// so Present()'s touch-fallback skips its safety clear.
	INDEX_API void RenderDrawData(ImDrawData* drawData, unsigned short viewId);

	// Releases ImGui's wgpu resources (pipeline, bind groups, font atlas
	// texture). Runs before ImGui::DestroyContext.
	INDEX_API void Shutdown();

	// ── Multi-viewport renderer hookup ──────────────────────────────────
	// Wire the Renderer_CreateWindow / DestroyWindow / SetWindowSize /
	// RenderWindow / SwapBuffers handlers on ImGui::GetPlatformIO() so that
	// when ImGuiConfigFlags_ViewportsEnable is set, secondary ImGui windows
	// dragged out of the host viewport spawn real OS windows (via
	// imgui_impl_glfw's platform handlers) AND get rendered through their
	// own wgpu::Surface (via the engine's WebGPUBackend::ViewportSurface
	// API). Idempotent.
	//
	// Call after Init(). Must run on the same ImGuiContext the consumer
	// publishes via PackageImGuiBridge.
	INDEX_API void RegisterMultiViewportHandlers();

	// Reverse of RegisterMultiViewportHandlers. Clears the renderer-side
	// platform_io callbacks. ImGui will have already called Renderer_Destroy-
	// Window on every secondary viewport before Shutdown reaches here, so
	// no per-viewport teardown is needed here. Called from Shutdown().
	INDEX_API void UnregisterMultiViewportHandlers();

	// Canonical "make the next popup look like a native OS dialog" helper.
	// Call IMMEDIATELY before ImGui::BeginPopupModal / ImGui::Begin to
	// produce a real OS window that:
	//   * Lives in its own viewport regardless of position (NoAutoMerge)
	//     — doesn't snap into the host viewport when it would fit inside.
	//   * Uses the OS-native titlebar (clears NoDecoration that ImGui
	//     sets on popup viewports by default — ImGui then auto-hides
	//     its own ImGui-drawn titlebar when the OS provides one).
	//   * Is NOT TopMost / always-on-top (clears TopMost that ImGui sets
	//     on popup viewports by default). imgui_impl_glfw translates
	//     TopMost into GLFW_RESIZABLE=false → no WS_THICKFRAME → the
	//     "compact dialog" close-button rendering (chunky red X) instead
	//     of the normal app close button. We use owner-window relationship
	//     (set in MultiViewport_CreateWindow) for "stays above main
	//     window" semantics instead — owner is the right tool for that
	//     and scopes it to the main HWND only, not all apps.
	//   * Has no separate taskbar entry (NoTaskBarIcon) — combined with
	//     the owner relationship, dialogs don't fragment the app in the
	//     taskbar / Alt-Tab list.
	//
	// Why every popup needs this: ImGui's popup logic unconditionally
	// applies NoDecoration + TopMost to popup viewports — those are
	// hardcoded, NOT driven by io.ConfigViewports*. The only override
	// path is a per-window ImGuiWindowClass.
	//
	// Lives in engine.dll so launcher, editor, packages, and projects
	// all use the same chrome-setup logic. No-op when multi-viewport
	// is disabled (the window class is still attached, just unused).
	INDEX_API void SetNextWindowAsNativeDialog();

	// Pump ImGui's secondary viewports. Call this each frame from the
	// consumer's OnPostRender, AFTER the main RenderDrawData call. No-op
	// when ImGuiConfigFlags_ViewportsEnable isn't set on the active
	// ImGuiIO.
	//
	// Order matters: this internally flushes the engine's frame encoder
	// before walking the viewport list so the main window's queue.Write-
	// Buffer uploads (vertex/index/uniform data for its ImGui draw) land
	// atomically with the main draws — BEFORE per-viewport renders queue
	// their own uploads to the same shared ImGui_ImplWGPU FrameResources
	// slot. Without this ordering, viewport WriteBuffer copies would
	// stomp the main window's pending uploads (Dawn's uploader is process-
	// wide; pending copies all flush on each Submit). The main surface
	// still presents later in Window::SwapBuffers — the flush leaves
	// the per-frame surface texture alive for that present.
	//
	// Exported via INDEX_API so the launcher / editor / runtime can drive
	// the per-frame multi-viewport pump without taking a direct dependency
	// on the engine-internal WebGPUBackend namespace.
	INDEX_API void RenderPlatformWindowsDefault();

}  // namespace Index::ImGuiImplWebGPU
