#include "RuntimeImGuiHost.hpp"

#include "Core/Window.hpp"
#include "Serialization/Path.hpp"

#include <filesystem>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

namespace Axiom {

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

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

		// Default ProggyClean covers ASCII; merge NotoSans for Latin-1
		// supplement so splash text / runtime overlays don't render
		// '?' for codepoints 0xA0-0xFF (umlauts, accented vowels,
		// ©/®/°/±/·/etc.). Mirrors the editor's font setup in
		// ImGuiContextLayer.cpp. Skipped silently if the bundled
		// font isn't found — text falls back to '?' as before.
		io.Fonts->AddFontDefault();
		const std::string notoPath = Path::Combine(Path::ResolveAxiomAssets("Fonts"), "NotoSans-Regular.ttf");
		if (std::filesystem::exists(notoPath)) {
			ImFontConfig latinCfg;
			latinCfg.MergeMode = true;
			latinCfg.PixelSnapH = true;
			static const ImWchar latin1Range[] = { 0x00A0, 0x00FF, 0 };
			io.Fonts->AddFontFromFileTTF(notoPath.c_str(), 13.0f, &latinCfg, latin1Range);
		}

		if (!ImGui_ImplGlfw_InitForOpenGL(glfwWindow, true)) {
			ImGui::DestroyContext();
			return false;
		}
		if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
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

		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
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
			ImGui_ImplOpenGL3_NewFrame();
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
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		}
	}

} // namespace Axiom
