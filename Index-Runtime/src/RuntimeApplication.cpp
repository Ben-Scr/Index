#include <Index.hpp>
#include "Core/Application.hpp"
#include "RuntimeLogLayer.hpp"
#include "RuntimeProfilerLayer.hpp"
#include "RuntimeSplashLayer.hpp"
#include "RuntimeStatsLayer.hpp"
#include "Scene/SceneDefinition.hpp"
#include "Scene/SceneManager.hpp"

#include <Scene/EntityHelper.hpp>
#include <Serialization/SceneSerializer.hpp>
#include <Serialization/Path.hpp>
#include <Serialization/File.hpp>
#include <Project/ProjectManager.hpp>
#include <Project/IndexProject.hpp>

#include <Core/Version.hpp>
#include <Core/Window.hpp>
#include <filesystem>

using namespace Index;


class RuntimeApplication : public Index::Application {
public:
	ApplicationConfig GetConfiguration() const override {
		ApplicationConfig config;
		IndexProject* project = ProjectManager::GetCurrentProject();
		std::string title = project
			? project->Name + " - Index Runtime"
			: "Index Runtime Application " + std::string(IDX_VERSION);

		if (project) {
			config.WindowSpecification = WindowSpecification(
				project->BuildWidth, project->BuildHeight, title,
				project->BuildResizable, true, project->BuildFullscreen);
		} else {
			config.WindowSpecification = WindowSpecification(800, 800, title, true, true, false);
		}
		config.EnableAudio = true;
		config.EnablePhysics2D = true;
		return config;
	}

	void ConfigureScenes() override {
		// Runtime is always in "playing" state — set before scenes load
		// so that systems like AudioUpdateSystem know to trigger PlayOnAwake.
		Application::SetIsPlaying(true);

		IndexProject* project = ProjectManager::GetCurrentProject();
		std::string startupScene = "SampleScene";
		if (project) {
			if (!project->StartupScene.empty()) startupScene = project->StartupScene;
			else if (!project->LastOpenedScene.empty()) startupScene = project->LastOpenedScene;
		}

		// Helper: registers a scene definition with standard systems + OnLoad deserializer
		auto registerScene = [&](const std::string& sceneName) -> SceneDefinition& {
			auto& def = GetSceneManager()->RegisterScene(sceneName);

			// Load scene file in OnLoad callback — runs BEFORE Awake/Start,
			// so entities exist when systems initialize (e.g. PlayOnAwake).
			if (project) {
				std::string scenePath = project->GetSceneFilePath(sceneName);
				def.OnLoad([scenePath](Scene& scene) {
					if (File::Exists(scenePath))
						SceneSerializer::LoadFromFile(scene, scenePath);
				});
			}
			return def;
		};

		if (project && !project->BuildSceneList.empty()) {
			for (const auto& sceneName : project->BuildSceneList) {
				auto& def = registerScene(sceneName);
				if (sceneName == startupScene) def.SetAsStartupScene();
			}
			if (!GetSceneManager()->HasSceneDefinition(startupScene)) {
				registerScene(startupScene).SetAsStartupScene();
			}
		} else {
			registerScene(startupScene).SetAsStartupScene();
		}
	}

	~RuntimeApplication() override = default;

	void ConfigureLayers() override {
		// Splash screen pushed as an overlay BEFORE InitializeStartupScenes
		// runs so the splash is on the layer stack — and visible to the
		// preload-frame Application::Init renders right before the
		// (potentially seconds-long) blocking scene load. Pushing it in
		// Start() instead would have meant the entire scene-load + Awake/
		// Start ran with a black window before the splash ever showed.
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (project && project->SplashScreen.Enabled) {
			PushOverlay<RuntimeSplashLayer>("RuntimeSplash");
		}
	}

	void Start() override {
		if (!ProjectManager::GetCurrentProject()) {
			EntityHelper::CreateCamera2DEntity();
		}

		IndexProject* project = ProjectManager::GetCurrentProject();

		// Push the runtime profiler layer only when the project opted into it.
		// (When --no-profiler was passed at premake time, the layer is built
		// as a no-op shell, so this push costs essentially nothing.)
		if (project && project->Profiler.EnableInRuntime) {
			PushLayer<RuntimeProfilerLayer>("RuntimeProfiler");
		}

		// Push the F6 stats overlay when the project opts in (default true).
		// Layers share the runtime ImGui context via RuntimeImGuiHost, so
		// pushing both this and the profiler layer above is fine — push order
		// doesn't matter for ImGui hosting, but it DOES matter for stacking:
		// stats is pushed first so it renders first this frame, and
		// RuntimeLogLayer reads the stats layer's last-rendered height to
		// position itself directly below.
		const bool showStats = project ? project->ShowRuntimeStats : true;
		if (showStats) {
			PushLayer<RuntimeStatsLayer>("RuntimeStats");
		}

		// Push the F7 log overlay when the project opts in (default true).
		// Stacks below the stats overlay when both visible.
		const bool showLogs = project ? project->ShowRuntimeLogs : true;
		if (showLogs) {
			PushLayer<RuntimeLogLayer>("RuntimeLogs");
		}
	}

	void Update() override {}
	void FixedUpdate() override {}
	void OnPaused() override {}
	void OnQuit() override {}
};


Index::Application* Index::CreateApplication() {
	// Auto-detect project: look for index-project.json next to the executable
	std::string exeDir = Path::ExecutableDir();
	if (IndexProject::Validate(exeDir)) {
		auto project = std::make_unique<IndexProject>(IndexProject::Load(exeDir));
		IDX_CORE_INFO_TAG("Runtime", "Loaded project: {} ({})", project->Name, project->RootDirectory);
		ProjectManager::SetCurrentProject(std::move(project));
	}
	else {
		// E30: no project file next to the executable — surface the fallback so
		// a packaging mistake (missing index-project.json) is visible in logs.
		IDX_CORE_WARN_TAG("Runtime", "index-project.json not found at '{}'; falling back to built-in sample scene", exeDir);
	}

	return new RuntimeApplication();
}

#include <EntryPoint.hpp>
