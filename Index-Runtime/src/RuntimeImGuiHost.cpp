#include "RuntimeImGuiHost.hpp"

#include "Core/Window.hpp"
#include "Gui/ImGuiFonts.hpp"
#include "Packages/PackageImGuiBridge.hpp"

#include <algorithm>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
// Must use engine.dll's WebGPU backend (GLFW_NO_API window has no GL context).
#include "Gui/ImGuiImplWebGPU.hpp"

namespace Index {

	namespace {
		int  s_RefCount        = 0; // Acquire/Release nesting depth
		bool s_Initialized     = false;
		int  s_FrameOpenCount  = 0; // BeginFrame/EndFrame nesting in this frame
	}

	bool RuntimeImGuiHost::Acquire(Window* window) {
		if (s_Initialized) {
			++s_RefCount;
			return true;
		}
		if (!window) return false;
		GLFWwindow* glfwWindow = window->GetGLFWWindow();
		if (!glfwWindow) return false;

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		// Publish context+allocators so engine.dll's static ImGui copy syncs to our context on every backend entry point.
		{
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

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

		float xScale = 1.0f, yScale = 1.0f;
		glfwGetWindowContentScale(glfwWindow, &xScale, &yScale);
		LoadIndexImGuiFont(io, std::max(1.0f, xScale));

		if (!ImGui_ImplGlfw_InitForOther(glfwWindow, true)) {
			ImGui::DestroyContext();
			return false;
		}
		if (!ImGuiImplWebGPU::Init()) {
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext();
			return false;
		}

		s_Initialized = true;
		++s_RefCount;
		return true;
	}

	void RuntimeImGuiHost::Release() {
		if (s_RefCount == 0) return;
		--s_RefCount;
		if (s_RefCount > 0) return;
		if (!s_Initialized) return;

		ImGuiImplWebGPU::Shutdown();
		ImGui_ImplGlfw_Shutdown();
		PackageImGuiBridge::Clear();
		ImGui::DestroyContext();
		s_Initialized = false;
		s_FrameOpenCount = 0;
	}

	bool RuntimeImGuiHost::IsInitialized() {
		return s_Initialized;
	}

	void RuntimeImGuiHost::BeginFrame() {
		if (!s_Initialized) return;
		if (s_FrameOpenCount == 0) {
			ImGuiImplWebGPU::NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();
		}
		++s_FrameOpenCount;
	}

	void RuntimeImGuiHost::EndFrame() {
		if (!s_Initialized) return;
		if (s_FrameOpenCount == 0) return;
		--s_FrameOpenCount;
		if (s_FrameOpenCount == 0) {
			ImGui::Render();
			ImGuiImplWebGPU::RenderDrawData(ImGui::GetDrawData(), /*viewId*/ 0xFFFFu);
		}
	}

} // namespace Index
