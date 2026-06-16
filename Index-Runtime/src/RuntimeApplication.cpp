#include <Index.hpp>
#include "Core/Application.hpp"
#include "RuntimeLogLayer.hpp"
#include "RuntimeProfilerLayer.hpp"
#include "RuntimeSplashLayer.hpp"
#include "RuntimeStatsLayer.hpp"
#include "Scene/SceneDefinition.hpp"
#include "Scene/SceneManager.hpp"

#include <Collections/AspectRatio.hpp>
#include <Scene/EntityHelper.hpp>
#include <Serialization/SceneSerializer.hpp>
#include <Serialization/Path.hpp>
#include <Serialization/File.hpp>
#include <Project/ProjectManager.hpp>
#include <Project/IndexProject.hpp>
#include <Assets/AssetRegistry.hpp>

#include <Core/Version.hpp>
#include <Core/Window.hpp>

using namespace Index;
  
static std::string s_ProjectPath;


class RuntimeApplication : public Index::Application {
public:
	ApplicationConfig GetConfiguration() const override {
		ApplicationConfig config;
		IndexProject* project = ProjectManager::GetCurrentProject();

		const char* buildConfig =
#if defined(IDX_DEBUG)
			"Debug";
#else
			(project && project->ActiveBuildProfile == IndexProject::BuildProfile::Release)
				? "Release"
				: "Development";
#endif

		std::string title = project
			? project->Name + " - " + buildConfig
			: "Index Runtime Application " + std::string(IDX_VERSION);

		if (project) {
			config.WindowSpecification = WindowSpecification(
				project->BuildWidth, project->BuildHeight, title,
				project->BuildResizable, true, project->BuildFullscreen);
			config.WindowSpecification.MinWidth  = project->BuildMinWidth;
			config.WindowSpecification.MinHeight = project->BuildMinHeight;
			config.WindowSpecification.MaxWidth  = project->BuildMaxWidth;
			config.WindowSpecification.MaxHeight = project->BuildMaxHeight;
			// Selects how Window::Create realises fullscreen when
			// BuildFullscreen is true. Has no effect for windowed builds.
			config.WindowSpecification.FullscreenMode = project->BuildFullscreenMode;
			// Resolves the stored preset label ("16:9" etc.) to a float
			// aspect. "Free Aspect" / unknown labels resolve to 0.0f
			// which the Window treats as "no lock".
			config.WindowSpecification.AspectLock = AspectRatioFromLabel(project->BuildAspect);
			SetRunInBackground(project->BuildRunInBackground);
		} else {
			config.WindowSpecification = WindowSpecification(800, 800, title, true, true, false);
			SetRunInBackground(false);
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

		auto registerScene = [&](const std::string& sceneName) -> SceneDefinition& {
			auto& def = GetSceneManager()->RegisterScene(sceneName);

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
		// MUST push before InitializeStartupScenes so the splash is visible during the blocking scene load; pushing in Start() would show a black window until load completes.
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

		const bool showStats = project ? project->ShowRuntimeStats : true;
		if (showStats) {
			PushLayer<RuntimeStatsLayer>("RuntimeStats");
		}

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
	const Application::CommandLineArgs args = Application::GetCommandLineArgs();
	for (int i = 1; i < args.Count; i++) {
		const char* rawArg = args[i];
		if (!rawArg) {
			continue;
		}

		std::string arg(rawArg);
		if (arg.rfind("--project=", 0) == 0) {
			s_ProjectPath = arg.substr(10);
			if (s_ProjectPath.size() >= 2 && s_ProjectPath.front() == '"' && s_ProjectPath.back() == '"') {
				s_ProjectPath = s_ProjectPath.substr(1, s_ProjectPath.size() - 2);
			}
		}
	}

	// Auto-detect project: look for project.json next to the executable
	const std::string projectRoot = s_ProjectPath.empty() ? Path::ExecutableDir() : s_ProjectPath;
	if (IndexProject::Validate(projectRoot)) {
		auto project = std::make_unique<IndexProject>(IndexProject::Load(projectRoot));
		IDX_CORE_INFO_TAG("Runtime", "Loaded project: {} ({})", project->Name, project->RootDirectory);
		// assetsImmutable=true: a shipped game's Assets/ is read-only, so the registry trusts the
		// shipped manifest and skips the directory walk (set inside SetCurrentProject/engine.dll so it
		// reaches the registry copy that actually runs the rebuild; ResolvePath still existence-checks).
		ProjectManager::SetCurrentProject(std::move(project), /*assetsImmutable=*/true);
	}
	else {
		// E30: no project file next to the executable — surface the fallback so
		// a packaging mistake (missing project.json) is visible in logs.
		IDX_CORE_WARN_TAG("Runtime", "project.json not found at '{}'; falling back to built-in sample scene", projectRoot);
	}

	return new RuntimeApplication();
}

#include <EntryPoint.hpp>
