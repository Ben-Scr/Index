#pragma once

struct GLFWwindow;
struct ImGuiIO;

namespace Index::EditorRuntime::ImGuiContextSetup {

	// Free helpers used by both Index-Launcher's and Index-Editor's
	// ImGuiContextLayer::OnAttach. Each captures one of the byte-identical
	// blocks the two layers shared before extraction. Kept as free
	// functions (not a base class) because the rest of each layer
	// genuinely diverges — see plan notes for the design rationale.

	// Publish the active ImGui context + allocator to engine.dll and any
	// loaded packages via PackageImGuiBridge. Must be called AFTER
	// ImGui::CreateContext.
	void PublishImGuiContextToPackages();

	// Apply the docking + multi-viewport ConfigFlags / BackendFlags both
	// binaries set. Does NOT touch IniSavingRate or IniFilename — those
	// are caller-specific.
	void ApplyCommonIoConfigFlags(ImGuiIO& io);

	// Initialize the GLFW + WebGPU ImGui backends and register the
	// multi-viewport renderer handlers. Asserts (IDX_VERIFY) on failure.
	void InitBackends(GLFWwindow* window);

	// Return the DPI scale captured from the window's monitor (≥ 1.0).
	// Caller stores it for ScaleAllSizes / font sizing.
	float CaptureDpiScale(GLFWwindow* window);

	// Shut down the backends in the order both binaries use: WebGPU →
	// GLFW → PackageImGuiBridge → ImGui::DestroyContext.
	void ShutdownBackends();

	// Drive a new ImGui frame: WebGPU NewFrame → GLFW NewFrame →
	// ImGui::NewFrame.
	void NewFrame();

	// Finalize and render the current ImGui frame, then pump secondary
	// OS windows for multi-viewport.
	void RenderAndPresent();

}
