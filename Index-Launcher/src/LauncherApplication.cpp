#include <Index.hpp>

#include "Core/Application.hpp"
#include "Scene/SceneDefinition.hpp"
#include "Scene/SceneManager.hpp"
#include <Gui/ImGuiContextLayer.hpp>
#include <Systems/LauncherLayer.hpp>
#include <Core/Version.hpp>

using namespace Index;

class LauncherApplication : public Application {
public:
	ApplicationConfig GetConfiguration() const override {
		ApplicationConfig config;
		config.WindowSpecification = WindowSpecification(900, 600, "Index Launcher " + std::string(IDX_VERSION), true, true, false);
		config.WindowSpecification.MinWidth  = 720;
		config.WindowSpecification.MinHeight = 480;
		// Keep native OS chrome; theme code updates its caption colors.
		config.WindowSpecification.CustomTitlebar = false;
		config.EnableAudio = false;
		config.EnableGuiRenderer = false;
		config.EnableGizmoRenderer = false;
		config.EnablePhysics2D = false;
		// Scripting off: launcher never runs game code, and holding ScriptCore.dll would block the editor from rebuilding it.
		config.EnableScripting = false;
		config.EnableRenderer2D = false;
		config.EnableTextureManager = false;
		config.EnablePackageHost = false;
		config.SetWindowIcon = true;
		config.Vsync = false;

		SetTargetFramerate(144.f);

		return config;
	}

	void ConfigureScenes() override {
		SceneDefinition& launcherScene = GetSceneManager()->RegisterScene("Launcher");
		launcherScene.SetAsStartupScene();
	}

	void ConfigureLayers() override {
		// ImGuiContextLayer must be pushed first — its OnPreRender / OnPostRender wrap
		// ImGui::NewFrame and ImGui::Render around the launcher's UI work.
		PushLayer<ImGuiContextLayer>();
		PushLayer<LauncherLayer>();
	}

	void Start() override {}
	void Update() override {}
	void FixedUpdate() override {}
	void OnPaused() override {}
	void OnQuit() override {}
};

Index::Application* Index::CreateApplication() {
	return new LauncherApplication();
}

#include <EntryPoint.hpp>
