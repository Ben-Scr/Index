#include <Index.hpp>

#include "Core/Application.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Scene/EntityHelper.hpp"
#include "Scene/SceneDefinition.hpp"
#include "Scene/SceneManager.hpp"
#include <Gui/ImGuiContextLayer.hpp>
#include <Systems/ImGuiEditorLayer.hpp>
#include <Systems/GizmosDebugSystem.hpp>
#include <Project/ProjectManager.hpp>
#include <Project/IndexProject.hpp>
#include <Serialization/SceneSerializer.hpp>
#include <Serialization/File.hpp>
#include <Core/Version.hpp>
#include "Editor/EditorComponentRegistration.hpp"

using namespace Index;

static std::string s_ProjectPath;

class EditorApplication : public Application {
public:
	EditorApplication() { 
		SetEditorHost(true);
	}

	ApplicationConfig GetConfiguration() const override {
		ApplicationConfig config;
		std::string title = "Index Editor " + std::string(IDX_VERSION);
		if (ProjectManager::HasProject())
			title += " - " + ProjectManager::GetCurrentProject()->Name;
		config.WindowSpecification = WindowSpecification(0, 0, title, true, true, true, true);
		config.WindowSpecification.MinWidth  = 1024;
		config.WindowSpecification.MinHeight = 600;
		// Keep native OS chrome; theme code updates its caption colors.
		config.WindowSpecification.CustomTitlebar = false;
		config.EnableAudio = true;
		config.EnableGizmoRenderer = true;
		config.EnableGuiRenderer = true;
		config.EnablePhysics2D = true;
		config.SetWindowIcon = true;
		// No F11 exclusive fullscreen: it takes over the display and breaks the
		// multi-window editor UI (detached panels / dialogs get covered).
		config.EnableFullscreenToggle = false;
		SetRunInBackground(true);
		return config;
	}

	void ConfigureScenes() override {
		Application::SetIsPlaying(false);

		SceneDefinition& editorScene = GetSceneManager()->RegisterScene("SampleScene");
		editorScene.OnLoad([](Scene& scene) {
			IndexProject* project = ProjectManager::GetCurrentProject();
			if (project) {
				const std::string sceneName = project->LastOpenedScene;
				const std::string scenePath = project->GetSceneFilePath(sceneName);
				if (File::Exists(scenePath)) {
					SceneSerializer::LoadFromFile(scene, scenePath);
					return;
				}
				if (!sceneName.empty()) {
					IDX_WARN_TAG("Editor",
						"Last opened scene '{}' could not be loaded — no scene file resolves under the project. Starting a fresh scene.",
						sceneName);
				}
			}

			EntityHelper::CreateCamera2DEntity(scene);
		});
		editorScene.SetAsStartupScene();
	}

	void ConfigureLayers() override {
		// ImGuiContextLayer must be pushed first — its OnPreRender / OnPostRender wrap
		// ImGui::NewFrame and ImGui::Render around the other layers' UI work.
		PushLayer<ImGuiContextLayer>();
		PushLayer<ImGuiEditorLayer>();
		PushOverlay<GizmosDebugSystem>();
	}

	void Start() override {
		RegisterEditorComponentInspectors(*GetSceneManager());
	}
	void Update() override {}
	void FixedUpdate() override {}
	void OnPaused() override {}

	void OnQuit() override {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			return;
		}

		auto* sceneManager = GetSceneManager();
		if (!sceneManager) {
			return;
		}

		Scene* active = sceneManager->GetActiveScene();
		if (active) {
			project->LastOpenedScene = active->GetName();
			project->Save();
		}
	}
};

Index::Application* Index::CreateApplication() {
	// Mark this process as the editor host before SetCurrentProject runs below — it
	// gates editor-only engine systems (e.g. the PrefabTemplateCache .prefab watcher)
	// on Application::IsEditor(), and the Application instance doesn't exist yet.
	Application::SetEditorHost(true);

	// Parse --project="path" from command line
	const Application::CommandLineArgs args = Application::GetCommandLineArgs();

	for (int i = 1; i < args.Count; i++) {
		const char* rawArg = args[i];
		if (!rawArg) {
			continue;
		}

		std::string arg(rawArg);
		if (arg.rfind("--project=", 0) == 0) {
			s_ProjectPath = arg.substr(10);
			// Remove surrounding quotes if present
			if (s_ProjectPath.size() >= 2 && s_ProjectPath.front() == '"' && s_ProjectPath.back() == '"')
				s_ProjectPath = s_ProjectPath.substr(1, s_ProjectPath.size() - 2);
		}
	}

	if (!s_ProjectPath.empty() && IndexProject::Validate(s_ProjectPath)) {
		auto project = std::make_unique<IndexProject>(IndexProject::Load(s_ProjectPath));
		IDX_CORE_INFO_TAG("Editor", "Opening project: {} ({})", project->Name, project->RootDirectory);
		ProjectManager::SetCurrentProject(std::move(project));
	}

	if (!ProjectManager::HasProject()) {
		IDX_CORE_WARN_TAG("Editor", "No project specified. Use: Index-Editor.exe --project=\"path/to/project\"");
		IDX_CORE_WARN_TAG("Editor", "Starting with default fallback paths.");
	}

	return new EditorApplication();
}

#include <EntryPoint.hpp>
