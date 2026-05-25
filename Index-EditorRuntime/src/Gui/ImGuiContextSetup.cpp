#include "Gui/ImGuiContextSetup.hpp"

#include "Core/Assert.hpp"
#include "Gui/ImGuiImplWebGPU.hpp"
#include "Packages/PackageImGuiBridge.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>

#include <algorithm>

namespace Index::EditorRuntime::ImGuiContextSetup {

	void PublishImGuiContextToPackages() {
		// Engine.dll has its own static-linked copy of ImGui (so it can
		// host the ImGui WebGPU backend alongside wgpu::Device). Publish
		// our context + allocators so engine.dll's ImGui state — and any
		// package that links ImGui statically (every engine_core package
		// today) — can sync to our context on every backend entry point.
		// Without this, package-side ImGui calls would crash on a null
		// GImGui. See Index-Engine/src/Gui/ImGuiImplWebGPU.cpp
		// `SyncImGuiContextFromBridge`.
		ImGuiMemAllocFunc allocFn = nullptr;
		ImGuiMemFreeFunc  freeFn  = nullptr;
		void*             userData = nullptr;
		ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &userData);
		PackageImGuiBridge::Publish(
			reinterpret_cast<void*>(ImGui::GetCurrentContext()),
			reinterpret_cast<void*>(allocFn),
			reinterpret_cast<void*>(freeFn),
			userData);
	}

	void ApplyCommonIoConfigFlags(ImGuiIO& io) {
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		// Multi-viewport: undocked ImGui windows (and popups marked with
		// ImGuiViewportFlags_NoAutoMerge via a window class) spawn real OS
		// windows through imgui_impl_glfw's platform handlers + the
		// engine's WebGPUBackend::ViewportSurface renderer handlers
		// (registered in InitBackends below). imgui_impl_wgpu doesn't
		// advertise renderer-side multi-viewport support on its own — we
		// bolt it on in ImGuiImplWebGPU. The matching BackendFlag tells
		// ImGui's per-frame UpdatePlatformWindows traversal that the
		// renderer side is ready to drive secondary viewports;
		// imgui_impl_glfw sets PlatformHasViewports in its own Init.
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
		// Tell ImGui that secondary viewports have OS-provided decoration,
		// so it should NOT draw its own titlebar/close-button on top of
		// the native one (the chrome our engine asserts via
		// ApplyViewportChrome). Without this, ImGui's default
		// ConfigViewportsNoDecoration=true makes it draw an ImGui titlebar
		// + X inside every detached window — producing the double-X
		// problem (one native, one ImGui-drawn). Docked windows are
		// unaffected: this flag only governs WHAT VIEWPORTS GET; docked
		// tab strips with close buttons are rendered by a separate path.
		io.ConfigViewportsNoDecoration = false;
		// Taskbar hiding is handled by the owner-window relationship that
		// MultiViewport_CreateWindow sets on each viewport HWND (Windows
		// excludes owned windows from the taskbar automatically). We
		// deliberately do NOT set io.ConfigViewportsNoTaskBarIcon = true
		// here — that would push ImGui to set
		// ImGuiViewportFlags_NoTaskBarIcon on every secondary viewport,
		// which causes imgui_impl_glfw's Platform_ShowWindow to re-apply
		// WS_EX_TOOLWINDOW (stripping WS_THICKFRAME and triggering the
		// chunky "compact dialog" close-button rendering instead of the
		// normal app close button).
		// Edge-resize left at default (true). Forcing it false combined
		// with the transparent ResizeGrip colors in the theme made
		// undocked floating windows effectively non-resizable.
	}

	void InitBackends(GLFWwindow* window) {
		// GLFW_NO_API means there's no GL context; use ImGui's "Other"
		// GLFW init that doesn't bind one. The WebGPU backend lives
		// inside engine.dll (Index-Engine/src/Gui/ImGuiImplWebGPU.{hpp,cpp},
		// a thin wrapper around imgui_impl_wgpu), reachable via INDEX_API.
		IDX_VERIFY(ImGui_ImplGlfw_InitForOther(window, true),
			"Failed to init glfw for imgui (WebGPU backend)!");
		IDX_VERIFY(ImGuiImplWebGPU::Init(),
			"Failed to init WebGPU imgui backend (device not ready?)");

		// Wire renderer-side multi-viewport handlers AFTER ImGui_ImplWGPU
		// is up. RegisterMultiViewportHandlers binds Renderer_RenderWindow
		// / SwapBuffers etc. to platform_io. imgui_impl_glfw registers its
		// own platform-side handlers lazily on first NewFrame when
		// ViewportsEnable is set.
		ImGuiImplWebGPU::RegisterMultiViewportHandlers();
	}

	float CaptureDpiScale(GLFWwindow* window) {
		// One-time HiDPI scale captured from the window's monitor.
		// Callers apply it to the UI font and to the theme via
		// ScaleAllSizes. Mid-session monitor moves are not handled —
		// wire glfwSetWindowContentScaleCallback if that becomes a real
		// symptom.
		float xScale = 1.0f, yScale = 1.0f;
		glfwGetWindowContentScale(window, &xScale, &yScale);
		(void)yScale;
		return std::max(1.0f, xScale);
	}

	void ShutdownBackends() {
		ImGuiImplWebGPU::Shutdown();
		ImGui_ImplGlfw_Shutdown();
		PackageImGuiBridge::Clear();
		ImGui::DestroyContext();
	}

	void NewFrame() {
		ImGuiImplWebGPU::NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	void RenderAndPresent() {
		ImGui::Render();
		// viewId is vestigial on WebGPU (preserved in the API signature
		// for ABI stability); ImGuiImplWebGPU::RenderDrawData uses
		// whatever framebuffer RenderApi::BindFramebuffer last bound.
		ImGuiImplWebGPU::RenderDrawData(ImGui::GetDrawData(), /*viewId*/ 0xFFFFu);

		// Pump secondary OS windows when multi-viewport is enabled. No-op
		// otherwise. The engine.dll-side helper internally flushes the
		// main encoder before walking viewports so per-frame WriteBuffer
		// ordering across the shared ImGui_ImplWGPU FrameResources slot
		// is correct — see ImGuiImplWebGPU.hpp for the full rationale.
		ImGuiImplWebGPU::RenderPlatformWindowsDefault();
	}

}
