#include "Systems/LauncherLayer.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/SceneManager.hpp"
#include "Core/Application.hpp"
#include <Core/Version.hpp>
#include <Core/Log.hpp>
#include "Core/Platform/Win32BuildProgressWindow.hpp"
#include "Gui/CustomTitlebar.hpp"
#include "Gui/Icons.hpp"
#include "Gui/ImGuiContextLayer.hpp"
#include "Gui/ImGuiImplWebGPU.hpp"
#include "Localization/Localization.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/Directory.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Utils/Hash.hpp"
#include "Utils/PackageToolPath.hpp"
#include "Utils/Process.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Utils/Timer.hpp"
#include "Utils/StringHelper.hpp"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <limits>
#include <system_error>
#include <vector>

#ifdef IDX_PLATFORM_WINDOWS
#include <shellapi.h>
#include <shobjidl.h>
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif

namespace Index {

	// Restart is the only way to add CJK glyphs: the atlas is baked statically at startup (no RendererHasTextures).
	static std::string ResolveOwnExecutablePath() {
#ifdef IDX_PLATFORM_WINDOWS
		std::vector<wchar_t> buf(MAX_PATH);
		for (;;) {
			DWORD copied = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
			if (copied == 0) return {};
			if (copied < buf.size()) break;
			buf.resize(buf.size() * 2);
			if (buf.size() > 65536) return {};
		}
		return std::filesystem::path(buf.data()).string();
#else
		char buf[PATH_MAX]{};
		ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if (len <= 0) return {};
		buf[len] = '\0';
		return std::string(buf);
#endif
	}

	// Spawns a new instance of the current binary and then quits the
	// running one. Used by the CJK-font restart prompt.
	static void RestartLauncherProcess() {
		const std::string exePath = ResolveOwnExecutablePath();
		if (exePath.empty()) {
			IDX_CORE_WARN_TAG("Launcher",
				"Cannot relaunch: failed to resolve own executable path.");
			return;
		}
		if (!Process::LaunchDetached({ exePath })) {
			IDX_CORE_WARN_TAG("Launcher",
				"Cannot relaunch: LaunchDetached failed for '{}'.", exePath);
			return;
		}
		IDX_CORE_INFO_TAG("Launcher", "Relaunching '{}'.", exePath);
		Application::Quit();
	}

	// ── Relative Time Formatting ────────────────────────────────────

	static std::string FormatRelativeTime(const std::string& iso8601) {
		if (iso8601.empty()) return "";

		std::tm tm{};
		std::istringstream ss(iso8601);
		ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
		if (ss.fail()) return "";

		std::time_t then = std::mktime(&tm);
		std::time_t now = std::time(nullptr);
		double seconds = std::difftime(now, then);

		if (seconds < 0) return "Now";
		if (seconds < 5) return "Now";
		if (seconds < 60) return std::to_string(static_cast<int>(seconds)) + "s ago";

		int minutes = static_cast<int>(seconds / 60);
		if (minutes < 60) return std::to_string(minutes) + "m ago";

		int hours = minutes / 60;
		if (hours < 24) return std::to_string(hours) + "h ago";

		int days = hours / 24;
		if (days < 30) return std::to_string(days) + "d ago";

		int months = days / 30;
		int remainingDays = days % 30;
		if (months < 12) {
			if (remainingDays > 0)
				return std::to_string(months) + "mo " + std::to_string(remainingDays) + "d ago";
			return std::to_string(months) + "mo ago";
		}

		int years = months / 12;
		int remainingMonths = months % 12;
		if (remainingMonths > 0)
			return std::to_string(years) + "y " + std::to_string(remainingMonths) + "mo ago";
		return std::to_string(years) + "y ago";
	}

	static std::string BuildManagedDefineConstantsForProject(const IndexProject& project, std::string_view primarySymbol) {
		std::string defineConstants;
		auto appendDefine = [&defineConstants](std::string_view define) {
			if (define.empty()) {
				return;
			}
			if (!defineConstants.empty()) {
				defineConstants += "%3B";
			}
			defineConstants += define;
		};

		appendDefine(primarySymbol);
		appendDefine(IndexProject::GetActiveBuildDefineConstant());
		appendDefine(project.GetActiveBuildProfileDefine());
		appendDefine(IndexProject::GetManagedPlatformDefine());
		for (const std::string& custom : project.CustomDefines) {
			appendDefine(custom);
		}
		return defineConstants;
	}

#ifdef IDX_PLATFORM_WINDOWS
	static std::wstring Utf8ToWide(std::string_view utf8) {
		if (utf8.empty()) {
			return {};
		}

		const int requiredChars = MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS,
			utf8.data(), static_cast<int>(utf8.size()),
			nullptr, 0);
		if (requiredChars <= 0) {
			return {};
		}

		std::wstring wide(static_cast<size_t>(requiredChars), L'\0');
		if (MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS,
			utf8.data(), static_cast<int>(utf8.size()),
			wide.data(), requiredChars) != requiredChars) {
			return {};
		}

		return wide;
	}

	static std::string FormatWindowsError(DWORD errorCode) {
		return std::error_code(static_cast<int>(errorCode), std::system_category()).message();
	}
#endif

	static void AddSaturating(std::uintmax_t& total, std::uintmax_t value) {
		const std::uintmax_t maxValue = std::numeric_limits<std::uintmax_t>::max();
		total = value > maxValue - total ? maxValue : total + value;
	}

	static std::uintmax_t CalculateProjectDirectorySize(const std::filesystem::path& root, std::string& errorMessage) {
		std::error_code ec;
		if (!std::filesystem::exists(root, ec) || ec) {
			errorMessage = ec ? ec.message() : "Project folder not found.";
			return 0;
		}

		if (!std::filesystem::is_directory(root, ec) || ec) {
			errorMessage = ec ? ec.message() : "Project path is not a folder.";
			return 0;
		}

		std::uintmax_t total = 0;
		std::vector<std::filesystem::path> pendingDirectories;
		pendingDirectories.push_back(root);

		while (!pendingDirectories.empty()) {
			std::filesystem::path current = std::move(pendingDirectories.back());
			pendingDirectories.pop_back();

			ec.clear();
			std::filesystem::directory_iterator it(
				current,
				std::filesystem::directory_options::skip_permission_denied,
				ec);
			if (ec) {
				if (current == root) {
					errorMessage = ec.message();
					return 0;
				}
				continue;
			}

			const std::filesystem::directory_iterator end;
			while (it != end) {
				const std::filesystem::directory_entry& entry = *it;

				std::error_code entryError;
				if (entry.is_symlink(entryError) && !entryError) {
					// Avoid counting linked directories outside the project or following cycles.
				}
				else {
					entryError.clear();
					if (entry.is_directory(entryError) && !entryError) {
						pendingDirectories.push_back(entry.path());
					}
					else {
						entryError.clear();
						if (entry.is_regular_file(entryError) && !entryError) {
							entryError.clear();
							const std::uintmax_t fileSize = entry.file_size(entryError);
							if (!entryError) {
								AddSaturating(total, fileSize);
							}
						}
					}
				}

				ec.clear();
				it.increment(ec);
				if (ec) {
					break;
				}
			}
		}

		return total;
	}

	static bool IsSafeProjectDeleteRoot(const std::filesystem::path& path) {
		if (path.empty()) {
			return false;
		}

		std::error_code ec;
		std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
		if (ec) {
			return false;
		}

		absolutePath = absolutePath.lexically_normal();
		if (!absolutePath.has_filename() || absolutePath == absolutePath.root_path()) {
			return false;
		}

		// Require 2+ components past the root to block disasters like C:\Windows or /usr.
		std::filesystem::path postRoot = absolutePath.lexically_relative(absolutePath.root_path());
		std::size_t componentCount = 0;
		for (const auto& part : postRoot) {
			if (!part.empty() && part != ".") {
				++componentCount;
			}
		}
		if (componentCount < 2) {
			return false;
		}

		// Defense-in-depth: block system folder names even if depth check passes (e.g. C:\Windows\Temp\FakeProject).
		static constexpr const char* k_SystemFolderNames[] = {
			"Windows", "Program Files", "Program Files (x86)", "ProgramData",
			"System Volume Information", "$Recycle.Bin",
			"usr", "etc", "bin", "sbin", "boot", "lib", "lib64", "dev",
			"proc", "sys", "var"
		};
		const std::string firstComponent = postRoot.begin()->string();
		for (const char* banned : k_SystemFolderNames) {
			if (firstComponent == banned) {
				return false;
			}
		}

		return true;
	}

	// Asset .meta files MUST NOT be gitignored — they carry the stable AssetGUID that all scene/prefab references depend on.
	static constexpr const char* k_ProjectGitignore = R"(# Index engine — project .gitignore
# Build artefacts
[Bb]in/
[Oo]bj/
[Bb]uild/
bin-int/
.index/build/

# Visual Studio per-user state
.vs/
*.user
*.suo
*.sln.docstates
*.userprefs

# Generated project files (regenerated by the launcher/editor on open)
*.sln
*.csproj.user
*.vcxproj
*.vcxproj.filters
*.vcxproj.user

# OS noise
.DS_Store
Thumbs.db
desktop.ini

# Editor / Launcher logs
*.log

# NuGet
project.lock.json
project.fragment.lock.json
artifacts/

# NOTE: Do NOT ignore Assets/**/*.meta — those files hold the stable
# AssetGUID for every asset and must be checked in alongside the asset.
)";

	static std::string FindGitExecutable() {
#ifdef IDX_PLATFORM_WINDOWS
		Process::Result whereResult = Process::Run({ "where", "git" });
		if (whereResult.Succeeded() && !whereResult.Output.empty()) {
			// Take only the first line — `where` prints one path per location.
			std::string firstLine = whereResult.Output;
			const size_t newline = firstLine.find_first_of("\r\n");
			if (newline != std::string::npos) {
				firstLine.resize(newline);
			}
			while (!firstLine.empty() && (firstLine.back() == ' ' || firstLine.back() == '\t')) {
				firstLine.pop_back();
			}
			if (!firstLine.empty()) {
				return firstLine;
			}
		}
		return {};
#else
		Process::Result whichResult = Process::Run({ "which", "git" });
		if (whichResult.Succeeded() && !whichResult.Output.empty()) {
			std::string trimmed = whichResult.Output;
			while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r'
				|| trimmed.back() == ' ' || trimmed.back() == '\t')) {
				trimmed.pop_back();
			}
			return trimmed;
		}
		return {};
#endif
	}

	// Does NOT stage/commit — leaves that to the user. Failure is non-fatal; caller logs and continues.
	static bool InitializeGitRepository(const std::string& projectRoot, std::string& outError) {
		outError.clear();

		const std::filesystem::path rootPath(projectRoot);
		if (projectRoot.empty() || !std::filesystem::exists(rootPath)) {
			outError = "Project root does not exist.";
			return false;
		}

		// Refuse to re-init an existing repo. The user may have a parent
		// repo (monorepo / multi-project workspace); writing .gitignore is
		// still safe, but a nested `git init` would be surprising.
		const std::filesystem::path dotGit = rootPath / ".git";
		const bool alreadyHasGit = std::filesystem::exists(dotGit);

		const std::string gitExe = FindGitExecutable();
		if (gitExe.empty() && !alreadyHasGit) {
			outError = "git executable not found on PATH.";
			return false;
		}

		const std::filesystem::path gitignorePath = rootPath / ".gitignore";
		if (!std::filesystem::exists(gitignorePath)) {
			std::error_code writeEc;
			std::ofstream stream(gitignorePath, std::ios::binary);
			if (!stream) {
				outError = "Failed to open .gitignore for writing.";
				return false;
			}
			stream << k_ProjectGitignore;
			if (!stream.good()) {
				outError = "Failed to write .gitignore.";
				return false;
			}
		}

		if (alreadyHasGit) {
			// Existing repo (or nested-repo scenario). We've already
			// dropped the .gitignore; treat this as a partial success.
			return true;
		}

		// -b main sets the branch without touching init.defaultBranch; fallback omits -b for git < 2.28.
		Process::Result initResult = Process::Run(
			{ gitExe, "init", "-b", "main" }, rootPath);
		if (!initResult.Succeeded()) {
			// Retry without -b for older git releases. The init still
			// works; the default branch comes out as whatever
			// init.defaultBranch resolves to.
			Process::Result fallback = Process::Run({ gitExe, "init" }, rootPath);
			if (!fallback.Succeeded()) {
				outError = "git init failed (exit code "
					+ std::to_string(fallback.ExitCode) + ").";
				return false;
			}
		}

		return true;
	}

	// ── Lifecycle ───────────────────────────────────────────────────

	void LauncherLayer::OnAttach(Application& app) {
		(void)app;
		Application::SetIsPlaying(false);

		// MUST be true: background polling drives open/create progress; without it the progress overlay freezes while the launcher is unfocused.
		Application::SetRunInBackground(true);

		// Settings drive the splash theme + UI defaults, so load them now; the
		// heavier registry scan is deferred to OnUpdate (runs behind the splash).
		LoadLauncherSettings();
		ApplyLauncherThemeIfNeeded();

		if (m_LanguageAuto) {
			Localization::SetLanguage(Localization::GetSystemLanguage());
		}

		const std::string defaultLocation = m_DefaultProjectsLocation.empty()
			? IndexProject::GetDefaultProjectsDir()
			: m_DefaultProjectsLocation;
		std::snprintf(m_NewProjectLocation, sizeof(m_NewProjectLocation), "%s",
			defaultLocation.c_str());

		m_Splash.Begin();
	}

	void LauncherLayer::OnUpdate(Application& app, float dt) {
		(void)app;
		(void)dt;
		// Heavy startup runs once, behind the visible splash. OnUpdate is NOT
		// called during Initialize's pre-loop refresh passes, so by the time it
		// fires the splash has already been presented. ValidateAll stats every
		// project directory on disk — the bulk of launcher startup cost.
		if (m_StartupWorkDone) {
			return;
		}
		m_Registry.Load();
		m_Registry.ValidateAll();
		m_Registry.Save();
		IDX_INFO_TAG("Launcher", "Index Launcher opened ({} project(s))", m_Registry.GetProjects().size());
		m_StartupWorkDone = true;
	}

	void LauncherLayer::OnPreRender(Application& app) {
		ApplyLauncherThemeIfNeeded();
		// MUST precede Begin/Text calls: NewFrame already ran, so scale takes effect immediately for this frame's metrics.
		ImGui::GetIO().FontGlobalScale = GetEffectiveFontScale();

		// Splash holds (static) until the deferred startup work is done, then
		// fades. While opaque, skip building the launcher UI (registry not loaded
		// yet); the fade draws on top via the foreground draw list.
		if (m_Splash.IsActive()) {
			m_Splash.Render(app, m_StartupWorkDone);
			if (m_Splash.IsCovering()) {
				return;
			}
		}

		Localization::Poll();
		PollCreateProjectTask();
		PollDuplicateProjectTask();
		const std::string titlebarText = IDX_TR("launcher.title") + std::string(" ") + std::string(IDX_VERSION);
		EditorRuntime::RenderTitlebar(EditorRuntime::TitlebarConfig{ titlebarText, /*Centered=*/false });
		RenderLauncherPanel();
		UpdateProgressPopup();
	}

	float LauncherLayer::GetEffectiveFontScale() const {
		switch (m_FontScale) {
			case FontScale::Auto:   return 1.0f;
			case FontScale::P75:    return 0.75f;
			case FontScale::P100:   return 1.0f;
			case FontScale::P125:   return 1.25f;
			case FontScale::P150:   return 1.50f;
			case FontScale::P175:   return 1.75f;
			case FontScale::P200:   return 2.00f;
		}
		return 1.0f;
	}

	bool LauncherLayer::IsEffectiveThemeDark() const {
		switch (m_ThemeSetting) {
			case ThemeSetting::Auto:
				return ImGuiContextLayer::GetSystemTheme() == ImGuiContextLayer::Theme::Dark;
			case ThemeSetting::Dark:
				return true;
			case ThemeSetting::Light:
				return false;
		}
		return true;
	}

	void LauncherLayer::ApplyLauncherThemeIfNeeded() {
		const bool dark = IsEffectiveThemeDark();
		if (m_LastAppliedDarkTheme.has_value() && *m_LastAppliedDarkTheme == dark) {
			return;
		}

		ImGuiContextLayer::ApplyIndexTheme(dark
			? ImGuiContextLayer::Theme::Dark
			: ImGuiContextLayer::Theme::Light);
		m_LastAppliedDarkTheme = dark;
	}

	void LauncherLayer::OnDetach(Application& app) {
		(void)app;
		ResetCreateProjectTask();
		ResetDuplicateProjectTask();
		if (m_OpenTask.Worker.joinable()) {
			m_OpenTask.Worker.join();
		}
		// Join before teardown: destructor would call terminate() if the thread is still joinable.
		if (m_AssetLibraryTask.Worker.joinable()) {
			m_AssetLibraryTask.Worker.join();
		}
		// Request stop on all workers before clearing so jthread destructors can overlap rather than serialize.
		for (auto& [_, task] : m_ProjectSizeTasks) {
			if (task) {
				task->Worker.request_stop();
			}
		}
		m_ProjectSizeTasks.clear();

		// MUST precede renderer teardown: Texture2D destructors access the WebGPU device.
		m_Splash.Shutdown();
		Icons::Shutdown();
	}

	void LauncherLayer::ResetCreateProjectTask(bool clearWorker) {
		if (clearWorker && m_CreateTask.Worker.joinable()) {
			m_CreateTask.Worker.join();
		}

		std::scoped_lock lock(m_CreateTask.Mutex);
		m_CreateTask.Worker = std::thread();
		m_CreateTask.CreatedProject.reset();
		m_CreateTask.Error.clear();
		m_CreateTask.Stage = "Idle";
		m_CreateTask.Progress = 0.0f;
		m_CreateTask.BuildExitCode = 0;
		m_CreateTask.Running = false;
		m_CreateTask.Finished = false;
		m_CreateTask.Success = false;
		m_CreateTask.InitialBuildSucceeded = true;
	}

	void LauncherLayer::StartCreateProjectAsync(const std::string& name, const std::string& location,
		const std::string& directoryName, bool initializeGit) {
		ResetCreateProjectTask();

		m_CreateError.clear();
		m_CloseCreatePopup = false;
		m_IsCreating = true;

		{
			std::scoped_lock lock(m_CreateTask.Mutex);
			m_CreateTask.Stage = "Preparing project...";
			m_CreateTask.Progress = 0.02f;
			m_CreateTask.Running = true;
		}

		m_CreateTask.Worker = std::thread([this, name, location, directoryName, initializeGit]() {
			auto updateProgress = [this](float progress, std::string_view stage) {
				std::scoped_lock lock(m_CreateTask.Mutex);
				m_CreateTask.Progress = progress;
				m_CreateTask.Stage = std::string(stage);
			};

			try {
				const std::string fullPath = Path::Combine(location, directoryName.empty() ? name : directoryName);
				if (std::filesystem::exists(fullPath)) {
					std::scoped_lock lock(m_CreateTask.Mutex);
					m_CreateTask.Error = "Directory already exists: " + fullPath;
					m_CreateTask.Stage = "Project creation failed";
					m_CreateTask.Finished = true;
					m_CreateTask.Running = false;
					m_CreateTask.Success = false;
					return;
				}

				updateProgress(0.05f, "Creating project...");
				IndexProject project = IndexProject::Create(name, location, directoryName, updateProgress);

				// Project-local packages affect the engine solution, so we regenerate. If
				// the project has no packages, regen is still cheap and keeps the solution
				// in sync with the latest engine package set — no rebuild follows.
				updateProgress(0.55f, "Regenerating engine solution...");
				IndexProject::RegenerateResult regen = IndexProject::RegenerateSolutionForProject(project.RootDirectory);
				if (!regen.Succeeded && regen.ExitCode != -1) {
					IDX_WARN_TAG("Launcher", "Solution regeneration returned non-zero ({}); continuing anyway.", regen.ExitCode);
				}

				// Package MSBuild step removed: Index.sln is engine-only; cloud-installed packages arrive as prebuilt DLLs via IndexPackageInstaller.
				updateProgress(0.86f, "Compiling starter scripts...");
				const std::string buildConfiguration = IndexProject::GetActiveBuildConfiguration();
				Process::Result buildResult = Process::Run({
					"dotnet",
					"build",
					project.CsprojPath,
					"-c", buildConfiguration,
					"--nologo",
					"-v", "q",
					"-nowarn:CS8632",
					"-p:DefineConstants=" + IndexProject::BuildManagedDefineConstants("INDEX_EDITOR")
					});

				if (initializeGit) {
					updateProgress(0.95f, "Initializing Git repository...");
					std::string gitError;
					if (!InitializeGitRepository(project.RootDirectory, gitError)) {
						IDX_WARN_TAG("Launcher",
							"Project created, but git initialization failed: {}",
							gitError.empty() ? std::string("unknown error") : gitError);
					}
				}

				std::scoped_lock lock(m_CreateTask.Mutex);
				m_CreateTask.CreatedProject = project;
				m_CreateTask.BuildExitCode = buildResult.ExitCode;
				m_CreateTask.InitialBuildSucceeded = buildResult.Succeeded();
				m_CreateTask.Progress = 1.0f;
				m_CreateTask.Stage = buildResult.Succeeded()
					? "Project ready."
					: "Project ready. Initial script build failed.";
				m_CreateTask.Finished = true;
				m_CreateTask.Running = false;
				m_CreateTask.Success = true;
			}
			catch (const std::exception& e) {
				std::scoped_lock lock(m_CreateTask.Mutex);
				m_CreateTask.Error = e.what();
				m_CreateTask.Stage = "Project creation failed";
				m_CreateTask.Finished = true;
				m_CreateTask.Running = false;
				m_CreateTask.Success = false;
			}
		});
	}

	void LauncherLayer::PollCreateProjectTask() {
		bool finished = false;
		bool success = false;
		bool initialBuildSucceeded = true;
		int buildExitCode = 0;
		std::string error;
		std::optional<IndexProject> createdProject;

		{
			std::scoped_lock lock(m_CreateTask.Mutex);
			finished = m_CreateTask.Finished;
			if (!finished) {
				return;
			}

			success = m_CreateTask.Success;
			initialBuildSucceeded = m_CreateTask.InitialBuildSucceeded;
			buildExitCode = m_CreateTask.BuildExitCode;
			error = m_CreateTask.Error;
			createdProject = m_CreateTask.CreatedProject;
		}

		if (m_CreateTask.Worker.joinable()) {
			m_CreateTask.Worker.join();
		}

		m_IsCreating = false;

		if (!success) {
			m_CreateError = error;
			ResetCreateProjectTask(false);
			return;
		}

		if (!createdProject.has_value()) {
			m_CreateError = "Project creation finished without a project result.";
			ResetCreateProjectTask(false);
			return;
		}

		m_Registry.AddProject(createdProject->Name, createdProject->RootDirectory);
		m_Registry.Save();

		if (!initialBuildSucceeded) {
			IDX_WARN_TAG("Launcher", "Project created, but the initial script build failed (exit code {}).", buildExitCode);
		}

		m_CloseCreatePopup = true;
		m_PendingCreatedProjectOpen = LauncherProjectEntry{ createdProject->Name, createdProject->RootDirectory };
		ResetCreateProjectTask(false);
	}

	// ── Main Panel ──────────────────────────────────────────────────

	void LauncherLayer::RenderLauncherPanel() {
		// Pull async open-task results before we draw anything.
		PollOpenProjectTask();

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		// Offset the panel below the custom titlebar (0 when disabled).
		// The titlebar window pinned at viewport->Pos owns the top rows;
		// the panel takes the remaining client area.
		const float titlebarH = EditorRuntime::GetTitlebarRowHeight();
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + titlebarH));
		ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - titlebarH));

		// NoBringToFrontOnFocus: keeps panel pinned to back so the floating gear overlay stays on top.
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking
			| ImGuiWindowFlags_NoBringToFrontOnFocus;

		ImGui::Begin("##Launcher", nullptr, flags);

		// Title text moved into the custom titlebar when it's active.
		if (titlebarH <= 0.0f) {
			ImGui::TextUnformatted(IDX_TR("launcher.title").c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("%s", IDX_VERSION);
			ImGui::Separator();
			ImGui::Spacing();
		}

		// m_IsOpening holds for 1.5s after the worker reports success so LastOpened is committed after the editor window appears.
		bool taskRunning = false;
		{
			std::scoped_lock lock(m_OpenTask.Mutex);
			taskRunning = m_OpenTask.Running;
		}
		if (m_IsOpening && !taskRunning) {
			float elapsed = std::chrono::duration<float>(
				std::chrono::steady_clock::now() - m_OpenStartTime).count();
			if (elapsed >= 1.5f) {
				m_IsOpening = false;
				if (!m_DeferredUpdatePath.empty()) {
					m_Registry.UpdateLastOpened(m_DeferredUpdatePath);
					m_Registry.Save();
					m_DeferredUpdatePath.clear();
				}
			}
		}

		PollAssetLibraryTask();

		if (ImGui::BeginTabBar("##LauncherTabs", ImGuiTabBarFlags_None)) {
			if (ImGui::BeginTabItem(IDX_TR("launcher.tab.my_projects").c_str())) {
				RenderMyProjectsTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(IDX_TR("launcher.tab.asset_library").c_str())) {
				// Kick off the initial fetch the first time the user opens
				// this tab. Subsequent visits hit the cache. Re-fetch is
				// triggered explicitly via the Refresh button.
				if (!m_AssetLibrary.Loaded && !m_AssetLibrary.FetchInFlight) {
					StartFetchAssetLibraryIndex();
				}
				RenderAssetLibraryTab();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		// Modals live at panel scope, after the tab bar — they're independent
		// of which tab is active and need to be reachable from both.
		RenderCreateProjectPopup();
		RenderDeleteProjectPopups();
		RenderSettingsPopup();
		RenderProjectInfoPopup();
		RenderErrorPopup();
		RenderRenameProjectPopup();
		RenderDuplicateProjectPopup();
		RenderAssetLibraryDetailModal();
		RenderAssetLibraryTrustModal();
		RenderAssetLibraryDoneModal();

		ImGui::End();

		// Standalone overlay window: SetCursorScreenPos inside the panel left the hit rect shadowed by child windows, so the button wouldn't click.
		{
			const ImGuiViewport* vp = ImGui::GetMainViewport();
			constexpr float k_GearSize = 36.0f;
			constexpr float k_GearIcon = 28.0f;
			constexpr float k_Margin   = 12.0f;
			ImGui::SetNextWindowPos(ImVec2(
				vp->Pos.x + vp->Size.x - k_GearSize - k_Margin,
				vp->Pos.y + vp->Size.y - k_GearSize - k_Margin));
			ImGui::SetNextWindowSize(ImVec2(k_GearSize, k_GearSize));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			// Intentionally no NoBringToFrontOnFocus: this overlay must reach the front over the back-pinned panel.
			constexpr ImGuiWindowFlags k_GearFlags =
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
				ImGuiWindowFlags_NoFocusOnAppearing |
				ImGuiWindowFlags_NoBackground;
			if (ImGui::Begin("##LauncherGearOverlay", nullptr, k_GearFlags)) {
				if (Icons::IconButtonBorderless("##LauncherSettingsGear",
					Icons::Type::Settings,
					ImVec2(k_GearSize, k_GearSize), k_GearIcon))
				{
					m_OpenSettingsPopup = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s",
						IDX_TR("launcher.action.settings").c_str());
				}
			}
			ImGui::End();
			ImGui::PopStyleVar(2);
		}
	}

	// ── My Projects Tab ──────────────────────────────────────────────

	void LauncherLayer::RenderMyProjectsTab() {
		float panelWidth = ImGui::GetContentRegionAvail().x;
		float rightColWidth = 200.0f;

		{
			Icons::TextIcon(Icons::Type::Search);
			char searchBuf[256];
			std::snprintf(searchBuf, sizeof(searchBuf), "%s", m_LauncherSearch.c_str());
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::InputTextWithHint("##LauncherSearch",
				IDX_TR("launcher.search.placeholder").c_str(),
				searchBuf, sizeof(searchBuf)))
			{
				m_LauncherSearch = searchBuf;
			}
			ImGui::Spacing();
		}

		const float strip_h = ImGui::GetFrameHeight();
		const float refreshW = strip_h;
		const float reverseW = strip_h;
		const float listColW = panelWidth - rightColWidth - 10.0f;

		ImGui::BeginGroup();
		const std::string sortLabelText = IDX_TR("launcher.sort.label");
		const float comboW = listColW - refreshW - reverseW
			- ImGui::GetStyle().ItemSpacing.x * 2.0f
			- ImGui::CalcTextSize(sortLabelText.c_str()).x - ImGui::GetStyle().ItemSpacing.x;
		ImGui::TextUnformatted(sortLabelText.c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(comboW);
		const std::string sortLastOpened = IDX_TR("launcher.sort.mode.last_opened");
		const std::string sortName = IDX_TR("launcher.sort.mode.name");
		const std::string sortCreated = IDX_TR("launcher.sort.mode.created");
		const char* k_SortLabels[] = { sortLastOpened.c_str(), sortName.c_str(), sortCreated.c_str() };
		int sortMode = static_cast<int>(m_SortMode);
		if (ImGui::Combo("##LauncherSort", &sortMode, k_SortLabels, IM_ARRAYSIZE(k_SortLabels))) {
			m_SortMode = static_cast<SortMode>(sortMode);
			SaveLauncherSettings();
		}
		ImGui::SameLine();
		const Icons::Type sortArrow = m_SortReverse ? Icons::Type::ArrowUp : Icons::Type::ArrowDown;
		if (Icons::IconButton("##SortReverse", sortArrow, ImVec2(reverseW, strip_h))) {
			m_SortReverse = !m_SortReverse;
			SaveLauncherSettings();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", m_SortReverse
				? IDX_TR("launcher.sort.tooltip.ascending").c_str()
				: IDX_TR("launcher.sort.tooltip.descending").c_str());
		}
		ImGui::SameLine();
		if (Icons::IconButton("##LauncherRefresh", Icons::Type::Refresh,
			ImVec2(refreshW, strip_h)))
		{
			RefreshProjectsList();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", IDX_TR("launcher.sort.tooltip.refresh").c_str());
		}
		ImGui::EndGroup();

		// Left column: project list
		ImGui::BeginChild("##ProjectList", ImVec2(listColW, 0), true);
		RenderProjectList();
		ImGui::EndChild();

		ImGui::SameLine();

		// Right column: actions (L4: removed selection-dependent buttons)
		ImGui::BeginChild("##Actions", ImVec2(rightColWidth, 0));

		if (Icons::IconButton("##LauncherAddProject", Icons::Type::Plus)) {
			ImGui::OpenPopup("##LauncherAddProjectPopup");
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", IDX_TR("launcher.action.add_project").c_str());
		}
		if (ImGui::BeginPopup("##LauncherAddProjectPopup")) {
			if (Icons::MenuItemWithIcon(Icons::Type::Plus,
				IDX_TR("launcher.action.new_blank").c_str()))
			{
				m_OpenCreatePopup = true;
				m_CreateError.clear();
				m_IsCreating = false;
				// Pre-fill the name field so a one-click "Create" works without
				// the user typing — matches Unity Hub / Godot defaults.
				std::snprintf(m_NewProjectName, sizeof(m_NewProjectName), "%s",
					IDX_TR("launcher.create.default_project_name").c_str());
				// Re-snap the location to whatever the user has set in
				// settings — they may have changed it since OnAttach.
				const std::string defaultLocation = m_DefaultProjectsLocation.empty()
					? IndexProject::GetDefaultProjectsDir()
					: m_DefaultProjectsLocation;
				std::snprintf(m_NewProjectLocation, sizeof(m_NewProjectLocation), "%s",
					defaultLocation.c_str());
			}
			if (Icons::MenuItemWithIcon(Icons::Type::FileManager,
				IDX_TR("launcher.action.new_from_disk").c_str()))
			{
				BrowseForExistingProject();
			}
			ImGui::EndPopup();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const LauncherProjectEntry* selected = GetSelectedProject();
		const bool hasSelection = (selected != nullptr);
		if (!hasSelection) ImGui::BeginDisabled();

		if (Icons::ButtonWithIcon(Icons::Type::Pen,
			IDX_TR("launcher.action.open_selected").c_str(), ImVec2(-1, 0)) && hasSelection)
		{
			OpenProject(*selected);
		}
		ImGui::Spacing();
		if (Icons::ButtonWithIcon(Icons::Type::Play,
			IDX_TR("launcher.action.execute_selected").c_str(), ImVec2(-1, 0)) && hasSelection)
		{
			ExecuteProject(*selected);
		}
		ImGui::Spacing();
		if (Icons::ButtonWithIcon(Icons::Type::Caret,
			IDX_TR("launcher.action.rename_selected").c_str(), ImVec2(-1, 0)) && hasSelection)
		{
			RequestProjectRename(*selected);
		}
		ImGui::Spacing();
		if (Icons::ButtonWithIcon(Icons::Type::Delete,
			IDX_TR("launcher.action.delete_selected").c_str(), ImVec2(-1, 0)) && hasSelection)
		{
			RequestProjectDelete(*selected);
		}

		if (!hasSelection) ImGui::EndDisabled();

		ImGui::EndChild();
	}

	// ── Project List ────────────────────────────────────────────────

	void LauncherLayer::RenderProjectList() {
		const auto sortedProjects = GetSortedProjectsView();

		if (sortedProjects.empty()) {
			ImGui::TextDisabled("%s", IDX_TR("launcher.list.empty.title").c_str());
			ImGui::TextDisabled("%s", IDX_TR("launcher.list.empty.subtitle").c_str());
			return;
		}

		const std::string lowerSearch = [&]() {
			std::string s = m_LauncherSearch;
			std::transform(s.begin(), s.end(), s.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}();
		auto matchesSearch = [&](const LauncherProjectEntry& e) {
			if (lowerSearch.empty()) return true;
			auto containsCI = [&](std::string_view hay) {
				std::string h(hay);
				std::transform(h.begin(), h.end(), h.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return h.find(lowerSearch) != std::string::npos;
			};
			return containsCI(e.Name) || containsCI(e.Path);
		};

		std::string removePath;
		int shownCount = 0;

		for (int i = 0; i < static_cast<int>(sortedProjects.size()); i++) {
			const auto& entry = *sortedProjects[i];
			if (!matchesSearch(entry)) continue;
			++shownCount;

			ImGui::PushID(i);

			std::string timeStr = FormatRelativeTime(entry.LastOpened);
			std::string sizeStr = GetProjectSizeDisplayText(entry);

			ImVec2 cursorPos = ImGui::GetCursorScreenPos();
			float rowWidth = ImGui::GetContentRegionAvail().x;
			float buttonWidth = 60.0f;
			float rowHeight = 68.0f;

			ImGui::InvisibleButton("##Row", ImVec2(rowWidth - buttonWidth - 8, rowHeight));
			const bool rowClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
			const bool rowRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
			const bool rowDoubleClicked = ImGui::IsItemHovered()
				&& ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
			if (rowClicked) {
				m_SelectedProjectPath = (m_SelectedProjectPath == entry.Path) ? std::string{} : entry.Path;
			}
			if (rowRightClicked) {
				m_SelectedProjectPath = entry.Path;
			}
			// Runs after the single-click toggle so that the second click of a
			// double-click (which would otherwise deselect) ends up leaving the
			// project selected before we hand off to OpenProject.
			if (rowDoubleClicked) {
				m_SelectedProjectPath = entry.Path;
				OpenProject(entry);
			}

			// Right-click context menu per item
			if (ImGui::BeginPopupContextItem("##RowCtx")) {
				if (Icons::MenuItemWithIcon(Icons::Type::Pen,
					IDX_TR("launcher.context.open_in_editor").c_str()))
				{
					OpenProject(entry);
				}

				if (Icons::MenuItemWithIcon(Icons::Type::Play,
					IDX_TR("launcher.context.execute_project").c_str()))
				{
					ExecuteProject(entry);
				}

				ImGui::Separator();

				if (Icons::MenuItemWithIcon(Icons::Type::FileManager,
					IDX_TR("launcher.context.open_in_explorer").c_str()))
				{
					OpenProjectInExplorer(entry);
				}

				if (Icons::MenuItemWithIcon(Icons::Type::Copy,
					IDX_TR("launcher.context.copy_path").c_str()))
				{
					ImGui::SetClipboardText(entry.Path.c_str());
				}

				ImGui::Separator();

				if (Icons::MenuItemWithIcon(Icons::Type::Caret,
					IDX_TR("launcher.context.rename").c_str()))
				{
					RequestProjectRename(entry);
				}

				if (Icons::MenuItemWithIcon(Icons::Type::Copy,
					IDX_TR("launcher.context.duplicate").c_str()))
				{
					RequestProjectDuplicate(entry);
				}

				if (Icons::MenuItemWithIcon(Icons::Type::Info,
					IDX_TR("launcher.context.info").c_str()))
				{
					m_PendingInfoProject = entry;
					m_OpenInfoPopup = true;
				}

				if (Icons::MenuItemWithIcon(Icons::Type::Minus,
					IDX_TR("launcher.context.remove_from_list").c_str()))
				{
					removePath = entry.Path;
				}

				if (Icons::MenuItemWithIcon(Icons::Type::Delete,
					IDX_TR("launcher.context.delete_project").c_str()))
				{
					RequestProjectDelete(entry);
				}
				ImGui::EndPopup();
			}

			// Draw hover/selected highlight. Selected rows use the active
			// header colour so they stay visible even when the cursor moves
			// off the row; hover stacks on top with the standard tint.
			const bool selected = (m_SelectedProjectPath == entry.Path);
			const bool hovered = ImGui::IsItemHovered();
			constexpr float k_RowRounding = 6.0f;
			if (selected) {
				ImGui::GetWindowDrawList()->AddRectFilled(
					cursorPos,
					ImVec2(cursorPos.x + rowWidth, cursorPos.y + rowHeight),
					ImGui::GetColorU32(ImGuiCol_HeaderActive),
					k_RowRounding);
			}
			else if (hovered) {
				ImGui::GetWindowDrawList()->AddRectFilled(
					cursorPos,
					ImVec2(cursorPos.x + rowWidth, cursorPos.y + rowHeight),
					ImGui::GetColorU32(ImGuiCol_HeaderHovered),
					k_RowRounding);
			}

			// Draw name
			ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + 8, cursorPos.y + 4));
			ImGui::TextUnformatted(entry.Name.c_str());

			// Draw path
			ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + 8, cursorPos.y + 24));
			ImGui::TextDisabled("%s", entry.Path.c_str());

			// Draw size with a placeholder while the background scan is running.
			ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + 8, cursorPos.y + 44));
			const std::string sizeRowText = Localization::Format("launcher.list.size_prefix", sizeStr);
			ImGui::TextDisabled("%s", sizeRowText.c_str());

			// Draw relative time in upper-right (offset left to leave room for button)
			if (!timeStr.empty()) {
				float timeWidth = ImGui::CalcTextSize(timeStr.c_str()).x;
				ImGui::SetCursorScreenPos(ImVec2(
					cursorPos.x + rowWidth - buttonWidth - timeWidth - 20,
					cursorPos.y + 4));
				ImGui::TextDisabled("%s", timeStr.c_str());
			}

			// Per-item Open button on the right side
			ImGui::SetCursorScreenPos(ImVec2(
				cursorPos.x + rowWidth - buttonWidth - 4,
				cursorPos.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f));
			/*if (ImGui::Button(IDX_TR("launcher.list.open").c_str(), ImVec2(buttonWidth, 0))) {
				Timer timer;
				OpenProject(entry);
				IDX_INFO_TAG("Project", "Opening Took: " + StringHelper::ToString(timer));
			}*/

			// Advance cursor past the row
			ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + rowHeight));

			ImGui::PopID();
		}

		// Search produced no matches — surface the same "empty" copy the
		// asset library tab uses so the UX stays consistent across tabs.
		if (shownCount == 0 && !lowerSearch.empty()) {
			ImGui::TextDisabled("%s", IDX_TR("launcher.asset_library.empty").c_str());
		}

		// Deferred: keyed by path because sortedProjects is a view and its indices don't map to registry storage.
		if (!removePath.empty()) {
			for (const auto& e : m_Registry.GetProjects()) {
				if (e.Path == removePath) {
					RemoveProjectFromList(e);
					break;
				}
			}
		}
	}

	std::shared_ptr<LauncherLayer::ProjectSizeTaskState> LauncherLayer::GetOrStartProjectSizeTask(const LauncherProjectEntry& entry) {
		auto existing = m_ProjectSizeTasks.find(entry.Path);
		if (existing != m_ProjectSizeTasks.end()) {
			return existing->second;
		}

		auto task = std::make_shared<ProjectSizeTaskState>();
		m_ProjectSizeTasks.emplace(entry.Path, task);

		std::weak_ptr<ProjectSizeTaskState> weakTask = task;
		task->Worker = std::jthread([weakTask = std::move(weakTask), path = entry.Path](std::stop_token stop) {
			std::string error;
			const std::uintmax_t bytes = CalculateProjectDirectorySize(std::filesystem::path(path), error);
			if (stop.stop_requested()) {
				return;
			}
			auto t = weakTask.lock();
			if (!t) {
				return;
			}
			std::scoped_lock lock(t->Mutex);
			t->Bytes = bytes;
			t->Error = std::move(error);
			t->Failed = !t->Error.empty();
			t->Finished = true;
		});

		return task;
	}

	std::string LauncherLayer::GetProjectSizeDisplayText(const LauncherProjectEntry& entry) {
		std::shared_ptr<ProjectSizeTaskState> task = GetOrStartProjectSizeTask(entry);

		std::scoped_lock lock(task->Mutex);
		if (!task->Finished) {
			return "calculating...";
		}
		if (task->Failed) {
			return "unavailable";
		}

		const std::uintmax_t maxSize = static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max());
		const std::size_t formattedSize = static_cast<std::size_t>(std::min(task->Bytes, maxSize));
		return StringHelper::ToIEC(formattedSize);
	}

	void LauncherLayer::RequestProjectDelete(const LauncherProjectEntry& entry) {
		m_PendingDeleteProject = entry;
		m_DeleteError.clear();
		m_OpenDeleteConfirmPopup = true;
		m_OpenDeleteFinalConfirmPopup = false;
	}

	void LauncherLayer::RequestProjectRename(const LauncherProjectEntry& entry) {
		m_PendingRenameProject = entry;
		// Pre-fill the editable buffer with the current name so a user who
		// just wants to tweak the casing or fix a typo doesn't have to retype.
		std::snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", entry.Name.c_str());
		m_OpenRenamePopup = true;
	}

	void LauncherLayer::RemoveProjectFromList(const LauncherProjectEntry& entry) {
		// Copy path before mutating: entry may reference a string inside m_Registry's vector (invalidated by remove_if).
		const std::string path = entry.Path;
		m_Registry.RemoveProject(path);
		m_Registry.Save();
		m_ProjectSizeTasks.erase(path);
		m_CreatedAtCache.erase(path);
		m_EngineVersionCache.erase(path);
		if (m_SelectedProjectPath == path) {
			m_SelectedProjectPath.clear();
		}
	}

	const LauncherProjectEntry* LauncherLayer::GetSelectedProject() const {
		if (m_SelectedProjectPath.empty()) return nullptr;
		for (const auto& entry : m_Registry.GetProjects()) {
			if (entry.Path == m_SelectedProjectPath) return &entry;
		}
		return nullptr;
	}

	void LauncherLayer::RenderRenameProjectPopup() {
		constexpr const char* k_RenameId = "###LauncherRenameProject";

		if (m_OpenRenamePopup) {
			ImGui::OpenPopup(k_RenameId);
			m_OpenRenamePopup = false;
		}

		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);

		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		const std::string title = IDX_TR("launcher.rename.title") + k_RenameId;
		if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return;
		}
		if (!m_PendingRenameProject.has_value()) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		ImGui::TextUnformatted(IDX_TR("launcher.rename.new_name").c_str());
		ImGui::SetNextItemWidth(-1);
		// Auto-focus on first frame so the user can immediately type without
		// clicking into the field. EnterReturnsTrue lets the Enter key
		// commit, matching the create-project flow.
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		const bool submitted = ImGui::InputText("##RenameField", m_RenameBuffer,
			sizeof(m_RenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);

		const std::string trimmed = m_RenameBuffer;
		const bool canCommit = IndexProject::IsValidProjectName(trimmed);

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		if (!canCommit) ImGui::BeginDisabled();
		const bool okClicked = ImGui::Button(IDX_TR("launcher.rename.save").c_str(), ImVec2(120, 0));
		if (!canCommit) ImGui::EndDisabled();

		if ((okClicked || submitted) && canCommit) {
			if (m_Registry.RenameProject(m_PendingRenameProject->Path, trimmed)) {
				m_Registry.Save();
			}
			m_PendingRenameProject.reset();
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button(IDX_TR("launcher.rename.cancel").c_str(), ImVec2(120, 0))) {
			m_PendingRenameProject.reset();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	// ── Duplicate Project ───────────────────────────────────────────

	void LauncherLayer::RequestProjectDuplicate(const LauncherProjectEntry& entry) {
		m_PendingDuplicateProject = entry;
		m_DuplicateError.clear();
		// Prefill "<Name> Copy" into the source's parent directory so the common
		// case (a sibling copy) needs no edits.
		const std::string suggested = entry.Name + " Copy";
		std::snprintf(m_DuplicateNameBuffer, sizeof(m_DuplicateNameBuffer), "%s", suggested.c_str());
		const std::filesystem::path parent = std::filesystem::path(entry.Path).parent_path();
		std::snprintf(m_DuplicateLocationBuffer, sizeof(m_DuplicateLocationBuffer), "%s", parent.string().c_str());
		m_OpenDuplicatePopup = true;
	}

	void LauncherLayer::RenderDuplicateProjectPopup() {
		constexpr const char* k_DuplicateId = "###LauncherDuplicateProject";

		if (m_OpenDuplicatePopup) {
			ImGui::OpenPopup(k_DuplicateId);
			m_OpenDuplicatePopup = false;
		}

		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);

		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		const std::string title = IDX_TR("launcher.duplicate.title") + k_DuplicateId;
		if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return;
		}

		// Closed from PollDuplicateProjectTask once the worker succeeds.
		if (m_CloseDuplicatePopup) {
			m_CloseDuplicatePopup = false;
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		if (!m_PendingDuplicateProject.has_value()) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		ImGui::TextUnformatted(IDX_TR("launcher.duplicate.new_name").c_str());
		ImGui::SetNextItemWidth(-1);
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::InputText("##DuplicateName", m_DuplicateNameBuffer, sizeof(m_DuplicateNameBuffer));

		ImGui::Spacing();
		ImGui::TextUnformatted(IDX_TR("launcher.duplicate.location").c_str());
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##DuplicateLocation", m_DuplicateLocationBuffer, sizeof(m_DuplicateLocationBuffer));

		const std::string directoryName = ApplyDirectoryNameConvention(m_DuplicateNameBuffer, m_DirectoryNameConvention);
		ImGui::Spacing();
		const std::string preview = Path::Combine(m_DuplicateLocationBuffer, directoryName);
		ImGui::TextDisabled("%s", Localization::Format("launcher.create.path_preview", preview).c_str());

		if (!m_DuplicateError.empty()) {
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", m_DuplicateError.c_str());
		}

		if (m_IsDuplicating) {
			std::string stage;
			float progress = 0.0f;
			{
				std::scoped_lock lock(m_DuplicateTask.Mutex);
				stage = m_DuplicateTask.Stage;
				progress = m_DuplicateTask.Progress;
			}
			ImGui::Spacing();
			ImGui::ProgressBar(progress, ImVec2(-1, 0));
			ImGui::TextDisabled("%s", stage.c_str());
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const bool canDuplicate = IndexProject::IsValidProjectName(m_DuplicateNameBuffer)
			&& IndexProject::IsValidProjectName(directoryName)
			&& !m_IsDuplicating;

		if (!canDuplicate) ImGui::BeginDisabled();
		if (ImGui::Button(IDX_TR("launcher.duplicate.duplicate_button").c_str(), ImVec2(120, 0))) {
			StartDuplicateProjectAsync(m_PendingDuplicateProject->Path,
				std::string(m_DuplicateNameBuffer),
				std::string(m_DuplicateLocationBuffer),
				directoryName);
		}
		if (!canDuplicate) ImGui::EndDisabled();

		ImGui::SameLine();
		if (m_IsDuplicating) ImGui::BeginDisabled();
		if (ImGui::Button(IDX_TR("launcher.duplicate.cancel").c_str(), ImVec2(120, 0))) {
			m_PendingDuplicateProject.reset();
			ImGui::CloseCurrentPopup();
		}
		if (m_IsDuplicating) ImGui::EndDisabled();

		ImGui::EndPopup();
	}

	void LauncherLayer::StartDuplicateProjectAsync(const std::string& sourceRoot, const std::string& newName,
		const std::string& location, const std::string& directoryName) {
		ResetDuplicateProjectTask();

		m_DuplicateError.clear();
		m_IsDuplicating = true;
		{
			std::scoped_lock lock(m_DuplicateTask.Mutex);
			m_DuplicateTask.Stage = "Preparing...";
			m_DuplicateTask.Progress = 0.02f;
			m_DuplicateTask.Running = true;
		}

		m_DuplicateTask.Worker = std::thread([this, sourceRoot, newName, location, directoryName]() {
			auto updateProgress = [this](float progress, std::string_view stage) {
				std::scoped_lock lock(m_DuplicateTask.Mutex);
				m_DuplicateTask.Progress = progress;
				m_DuplicateTask.Stage = std::string(stage);
			};

			try {
				IndexProject duplicated =
					IndexProject::Duplicate(sourceRoot, newName, location, directoryName, updateProgress);

				std::scoped_lock lock(m_DuplicateTask.Mutex);
				m_DuplicateTask.Result = duplicated;
				m_DuplicateTask.Progress = 1.0f;
				m_DuplicateTask.Stage = "Duplicate ready.";
				m_DuplicateTask.Finished = true;
				m_DuplicateTask.Running = false;
				m_DuplicateTask.Success = true;
			}
			catch (const std::exception& e) {
				std::scoped_lock lock(m_DuplicateTask.Mutex);
				m_DuplicateTask.Error = e.what();
				m_DuplicateTask.Stage = "Duplicate failed";
				m_DuplicateTask.Finished = true;
				m_DuplicateTask.Running = false;
				m_DuplicateTask.Success = false;
			}
		});
	}

	void LauncherLayer::PollDuplicateProjectTask() {
		bool finished = false;
		bool success = false;
		std::string error;
		std::optional<IndexProject> result;

		{
			std::scoped_lock lock(m_DuplicateTask.Mutex);
			finished = m_DuplicateTask.Finished;
			if (!finished) {
				return;
			}
			success = m_DuplicateTask.Success;
			error = m_DuplicateTask.Error;
			result = m_DuplicateTask.Result;
		}

		if (m_DuplicateTask.Worker.joinable()) {
			m_DuplicateTask.Worker.join();
		}

		m_IsDuplicating = false;

		if (!success || !result.has_value()) {
			m_DuplicateError = error.empty() ? std::string("Duplication failed.") : error;
			ResetDuplicateProjectTask(false);
			return;
		}

		m_Registry.AddProject(result->Name, result->RootDirectory);
		m_Registry.Save();
		RefreshProjectsList();

		m_PendingDuplicateProject.reset();
		m_CloseDuplicatePopup = true;
		ResetDuplicateProjectTask(false);
	}

	void LauncherLayer::ResetDuplicateProjectTask(bool clearWorker) {
		if (clearWorker && m_DuplicateTask.Worker.joinable()) {
			m_DuplicateTask.Worker.join();
		}

		std::scoped_lock lock(m_DuplicateTask.Mutex);
		m_DuplicateTask.Worker = std::thread();
		m_DuplicateTask.Result.reset();
		m_DuplicateTask.Error.clear();
		m_DuplicateTask.Stage = "Idle";
		m_DuplicateTask.Progress = 0.0f;
		m_DuplicateTask.Running = false;
		m_DuplicateTask.Finished = false;
		m_DuplicateTask.Success = false;
	}

	void LauncherLayer::RenderDeleteProjectPopups() {
		// Stable IDs (everything after `###`) so the translated title can
		// vary without losing the popup's identity across frames.
		constexpr const char* k_DeleteId = "###LauncherDeleteProject";
		constexpr const char* k_DeleteFinalId = "###LauncherDeleteProjectFinal";

		if (m_OpenDeleteConfirmPopup) {
			ImGui::OpenPopup(k_DeleteId);
			m_OpenDeleteConfirmPopup = false;
		}
		if (m_OpenDeleteFinalConfirmPopup) {
			ImGui::OpenPopup(k_DeleteFinalId);
			m_OpenDeleteFinalConfirmPopup = false;
		}

		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);

		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		const std::string deleteTitle = IDX_TR("launcher.delete.title") + k_DeleteId;
		if (ImGui::BeginPopupModal(deleteTitle.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			if (!m_PendingDeleteProject.has_value()) {
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}

			const LauncherProjectEntry& entry = *m_PendingDeleteProject;
			const std::string msg = Localization::Format("launcher.delete.message", entry.Name);
			ImGui::TextWrapped("%s", msg.c_str());
			ImGui::Spacing();
			ImGui::TextWrapped("%s", IDX_TR("launcher.delete.body").c_str());
			ImGui::Spacing();
			const std::string folder = Localization::Format("launcher.delete.folder_label", entry.Path);
			ImGui::TextWrapped("%s", folder.c_str());
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			if (ImGui::Button(IDX_TR("launcher.delete.continue").c_str(), ImVec2(140, 0))) {
				m_OpenDeleteFinalConfirmPopup = true;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();
			if (ImGui::Button(IDX_TR("launcher.delete.cancel").c_str(), ImVec2(120, 0))) {
				m_PendingDeleteProject.reset();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);

		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		const std::string deleteFinalTitle = IDX_TR("launcher.delete_final.title") + k_DeleteFinalId;
		if (ImGui::BeginPopupModal(deleteFinalTitle.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			if (!m_PendingDeleteProject.has_value()) {
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}

			const LauncherProjectEntry& entry = *m_PendingDeleteProject;
			const std::string msg = Localization::Format("launcher.delete_final.message", entry.Name);
			ImGui::TextWrapped("%s", msg.c_str());
			ImGui::Spacing();
			ImGui::TextWrapped("%s", IDX_TR("launcher.delete_final.body").c_str());
			ImGui::Spacing();
			const std::string folder = Localization::Format("launcher.delete.folder_label", entry.Path);
			ImGui::TextWrapped("%s", folder.c_str());

			if (!m_DeleteError.empty()) {
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", m_DeleteError.c_str());
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.12f, 0.12f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.18f, 0.18f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.08f, 0.08f, 1.0f));
			const bool deleteClicked = ImGui::Button(IDX_TR("launcher.delete_final.confirm").c_str(), ImVec2(180, 0));
			ImGui::PopStyleColor(3);

			if (deleteClicked && DeleteProjectFromDisk(entry)) {
				m_PendingDeleteProject.reset();
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();
			if (ImGui::Button(IDX_TR("launcher.delete.cancel").c_str(), ImVec2(120, 0))) {
				m_PendingDeleteProject.reset();
				m_DeleteError.clear();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	// ── Create Project Popup ────────────────────────────────────────

	void LauncherLayer::RenderCreateProjectPopup() {
		constexpr const char* k_CreateId = "###LauncherCreateProject";

		if (m_OpenCreatePopup) {
			ImGui::OpenPopup(k_CreateId);
			m_OpenCreatePopup = false;
		}

		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_Appearing);

		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();

		const std::string createTitle = IDX_TR("launcher.create.title") + k_CreateId;
		if (ImGui::BeginPopupModal(createTitle.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			if (m_CloseCreatePopup) {
				m_CloseCreatePopup = false;
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();

				if (m_PendingCreatedProjectOpen.has_value()) {
					OpenProject(*m_PendingCreatedProjectOpen);
					m_PendingCreatedProjectOpen.reset();
				}
				return;
			}

			ImGui::TextUnformatted(IDX_TR("launcher.create.project_name").c_str());
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText("##ProjName", m_NewProjectName, sizeof(m_NewProjectName));

			ImGui::Spacing();
			ImGui::TextUnformatted(IDX_TR("launcher.create.location").c_str());
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText("##ProjLocation", m_NewProjectLocation, sizeof(m_NewProjectLocation));

			ImGui::Spacing();
			const std::string directoryName = ApplyDirectoryNameConvention(m_NewProjectName, m_DirectoryNameConvention);
			std::string preview = Path::Combine(m_NewProjectLocation, directoryName);
			const std::string previewText = Localization::Format("launcher.create.path_preview", preview);
			ImGui::TextDisabled("%s", previewText.c_str());

			ImGui::Spacing();
			ImGui::Checkbox(IDX_TR("launcher.create.init_git").c_str(), &m_NewProjectInitializeGit);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", IDX_TR("launcher.create.init_git.tooltip").c_str());
			}

			if (!m_CreateError.empty()) {
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", m_CreateError.c_str());
			}

			// Create-project progress is surfaced via the shared OS-level
			// popup (Win32BuildProgressWindow), driven from UpdateProgressPopup.

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			bool canCreate = IndexProject::IsValidProjectName(m_NewProjectName)
				&& IndexProject::IsValidProjectName(directoryName)
				&& !m_IsCreating;
			if (!canCreate) ImGui::BeginDisabled();

			if (ImGui::Button(IDX_TR("launcher.create.create_button").c_str(), ImVec2(120, 0))) {
				Timer timer;
				StartCreateProjectAsync(std::string(m_NewProjectName),
					std::string(m_NewProjectLocation),
					directoryName,
					m_NewProjectInitializeGit);
				IDX_INFO_TAG("Project", "Queued async project creation in {}", StringHelper::ToString(timer));
			}

			if (!canCreate) ImGui::EndDisabled();

			ImGui::SameLine();
			if (m_IsCreating) {
				ImGui::BeginDisabled();
			}
			if (ImGui::Button(m_IsCreating
				? IDX_TR("launcher.create.working").c_str()
				: IDX_TR("launcher.create.cancel").c_str(), ImVec2(120, 0))) {
				ImGui::CloseCurrentPopup();
			}
			if (m_IsCreating) {
				ImGui::EndDisabled();
			}

			ImGui::EndPopup();
		}
	}

	// ── Open Project ────────────────────────────────────────────────
	void LauncherLayer::OpenProject(const LauncherProjectEntry& entry) {
		StartProjectLaunch(entry, false);
	}

	void LauncherLayer::ExecuteProject(const LauncherProjectEntry& entry) {
		StartProjectLaunch(entry, true);
	}

	// Launches the editor/runtime process but keeps the launcher open.
	void LauncherLayer::StartProjectLaunch(const LauncherProjectEntry& entry, bool launchRuntime) {
		if (m_IsOpening) return; // Already in post-launch grace overlay
		{
			std::scoped_lock lock(m_OpenTask.Mutex);
			if (m_OpenTask.Running) return; // Worker already busy
		}

#ifdef IDX_PLATFORM_WINDOWS
		// Blocks duplicate editor OR runtime for the same project; the two kinds are independent and may coexist.
		{
			auto it = m_RunningProjects.find(entry.Path);
			if (it != m_RunningProjects.end()) {
				DWORD& slot = launchRuntime ? it->second.RuntimePid : it->second.EditorPid;
				if (slot != 0) {
					HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, slot);
					if (hProc) {
						DWORD exitCode = 0;
						if (GetExitCodeProcess(hProc, &exitCode) && exitCode == STILL_ACTIVE) {
							CloseHandle(hProc);
							ShowError(IDX_TR("launcher.error.project_already_open"));
							return;
						}
						CloseHandle(hProc);
					}
					slot = 0;
				}
				if (it->second.EditorPid == 0 && it->second.RuntimePid == 0) {
					m_RunningProjects.erase(it);
				}
			}
		}
#endif

		// Initialize task state and spawn the worker. The render thread shows progress
		// from the task state each frame; PollOpenProjectTask collects results.
		{
			std::scoped_lock lock(m_OpenTask.Mutex);
			m_OpenTask.Entry = entry;
			m_OpenTask.LaunchRuntime = launchRuntime;
			m_OpenTask.Stage = "Preparing...";
			m_OpenTask.Progress = 0.02f;
			m_OpenTask.Error.clear();
			m_OpenTask.Running = true;
			m_OpenTask.Finished = false;
			m_OpenTask.Success = false;
#ifdef IDX_PLATFORM_WINDOWS
			m_OpenTask.SpawnedProcessId = 0;
#endif
		}

		if (m_OpenTask.Worker.joinable()) {
			m_OpenTask.Worker.join();
		}

		// Show the in-progress overlay immediately by setting m_IsOpening — the overlay
		// reads stage/progress from m_OpenTask and dismisses 1.5 s after the worker
		// reports success. m_OpeningProjectName is what the overlay shows underneath.
		m_IsOpening = true;
		m_OpeningProjectName = entry.Name;

		m_OpenTask.Worker = std::thread([this, entry, launchRuntime]() {
			OpenProjectWorkerBody(entry, launchRuntime);
		});
	}

	void LauncherLayer::OpenProjectWorkerBody(const LauncherProjectEntry& entry, bool launchRuntime) {
		auto setStage = [this](const std::string& stage, float progress) {
			std::scoped_lock lock(m_OpenTask.Mutex);
			m_OpenTask.Stage = stage;
			m_OpenTask.Progress = progress;
		};

		auto fail = [this](const std::string& error) {
			std::scoped_lock lock(m_OpenTask.Mutex);
			m_OpenTask.Stage = "Failed";
			m_OpenTask.Progress = 1.0f;
			m_OpenTask.Error = error;
			m_OpenTask.Finished = true;
			m_OpenTask.Running = false;
			m_OpenTask.Success = false;
		};

		setStage("Regenerating engine solution...", 0.10f);
		IndexProject::RegenerateResult regen = IndexProject::RegenerateSolutionForProject(entry.Path);
		if (!regen.Succeeded && regen.ExitCode != -1) {
			fail("Solution regen failed (premake exit code " + std::to_string(regen.ExitCode) + ").");
			return;
		}

		// Package MSBuild step removed: Index.sln is engine-only; cloud-installed packages arrive as prebuilt DLLs.
		if (launchRuntime) {
			setStage("Compiling runtime scripts...", 0.70f);
			if (!IndexProject::Validate(entry.Path)) {
				fail("Project file not found.");
				return;
			}

			IndexProject project = IndexProject::Load(entry.Path);
			if (std::filesystem::exists(project.CsprojPath)) {
				const std::string buildConfiguration = IndexProject::GetActiveBuildConfiguration();
				Process::Result buildResult = Process::Run({
					"dotnet",
					"build",
					project.CsprojPath,
					"-c", buildConfiguration,
					"--nologo",
					"-v", "q",
					"-p:DefineConstants=" + BuildManagedDefineConstantsForProject(project, "INDEX_BUILD")
				});
				if (!buildResult.Succeeded()) {
					fail("Runtime script build failed (dotnet exit code "
						+ std::to_string(buildResult.ExitCode) + ").");
					return;
				}
			}
		}

		setStage(launchRuntime ? "Launching runtime..." : "Launching editor...", 0.90f);

#ifdef IDX_PLATFORM_WINDOWS
		auto exeDir = std::filesystem::path(Path::ExecutableDir());
		const char* executableName = launchRuntime ? "Index-Runtime.exe" : "Index-Editor.exe";
		const char* siblingDirectoryName = launchRuntime ? "Index-Runtime" : "Index-Editor";

		auto targetExe = exeDir / executableName;
		if (!std::filesystem::exists(targetExe)) {
			targetExe = exeDir / ".." / siblingDirectoryName / executableName;
		}
		if (!std::filesystem::exists(targetExe)) {
			fail(std::string(executableName) + " not found.");
			return;
		}

		std::error_code canonicalError;
		std::filesystem::path resolvedTargetExe = std::filesystem::weakly_canonical(targetExe, canonicalError);
		if (canonicalError) {
			resolvedTargetExe = targetExe.lexically_normal();
		}

		const std::wstring projectPath = Utf8ToWide(entry.Path);
		if (projectPath.empty() && !entry.Path.empty()) {
			fail("Project path is not valid UTF-8.");
			return;
		}

		const std::wstring commandLine =
			L"\"" + resolvedTargetExe.native() + L"\" --project=\"" + projectPath + L"\"";
		std::vector<wchar_t> buf(commandLine.begin(), commandLine.end());
		buf.push_back(L'\0');

		const std::wstring workingDirectory = resolvedTargetExe.parent_path().native();

		STARTUPINFOW si{};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi{};

		if (!CreateProcessW(resolvedTargetExe.c_str(), buf.data(), nullptr, nullptr,
			FALSE, CREATE_NEW_PROCESS_GROUP, nullptr,
			workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &si, &pi))
		{
			const DWORD errorCode = GetLastError();
			fail(std::string("Failed to launch ") + (launchRuntime ? "runtime: " : "editor: ")
				+ FormatWindowsError(errorCode));
			return;
		}

		const DWORD spawnedPid = pi.dwProcessId;
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);

		IDX_INFO_TAG("Launcher", "{} project: {} at {}",
			launchRuntime ? "Executed" : "Opened", entry.Name, entry.Path);

		std::scoped_lock lock(m_OpenTask.Mutex);
		m_OpenTask.Stage = launchRuntime ? "Runtime launched" : "Editor launched";
		m_OpenTask.Progress = 1.0f;
		m_OpenTask.SpawnedProcessId = spawnedPid;
		m_OpenTask.Success = true;
		m_OpenTask.Finished = true;
		m_OpenTask.Running = false;
#else
		fail(launchRuntime
			? "Project execute is only implemented on Windows."
			: "Project open is only implemented on Windows.");
#endif
	}

	void LauncherLayer::PollOpenProjectTask() {
		bool finished = false;
		bool success = false;
		bool launchRuntime = false;
		std::string error;
		LauncherProjectEntry entry;
#ifdef IDX_PLATFORM_WINDOWS
		DWORD pid = 0;
#endif

		{
			std::scoped_lock lock(m_OpenTask.Mutex);
			if (!m_OpenTask.Finished) return;
			finished = true;
			success = m_OpenTask.Success;
			launchRuntime = m_OpenTask.LaunchRuntime;
			error = m_OpenTask.Error;
			entry = m_OpenTask.Entry;
#ifdef IDX_PLATFORM_WINDOWS
			pid = m_OpenTask.SpawnedProcessId;
#endif
			m_OpenTask.Finished = false;
		}

		if (m_OpenTask.Worker.joinable()) {
			m_OpenTask.Worker.join();
		}

		if (success) {
			// Editor process spawned; transition to the post-launch grace overlay so
			// the user sees the launch confirmation for a beat before the launcher
			// becomes interactive again.
			m_OpenStartTime = std::chrono::steady_clock::now();
			m_OpeningProjectName = entry.Name;
			m_DeferredUpdatePath = entry.Path;
#ifdef IDX_PLATFORM_WINDOWS
			auto& slots = m_RunningProjects[entry.Path];
			(launchRuntime ? slots.RuntimePid : slots.EditorPid) = pid;
#endif
		}
		else {
			const std::string msg = error.empty()
				? (launchRuntime ? std::string("Failed to execute project.") : std::string("Failed to open project."))
				: error;
			m_IsOpening = false;
			IDX_ERROR_TAG("Launcher", "{} project failed: {}",
				launchRuntime ? "Execute" : "Open", msg);
			ShowError(msg);
		}
	}

	void LauncherLayer::UpdateProgressPopup() {
		std::string title;
		std::string stage;
		float progress = 0.0f;
		bool show = false;

		// ── Open / Execute project ─────────────────────────────────
		{
			std::scoped_lock lock(m_OpenTask.Mutex);
			if (m_OpenTask.Running) {
				show = true;
				title = m_OpenTask.LaunchRuntime
					? IDX_TR("launcher.progress.executing_project")
					: IDX_TR("launcher.progress.opening_project");
				stage = m_OpenTask.Stage.empty()
					? IDX_TR("launcher.progress.working")
					: m_OpenTask.Stage;
				progress = m_OpenTask.Progress;
			}
		}

		// ── Create project ─────────────────────────────────────────
		if (!show) {
			std::scoped_lock lock(m_CreateTask.Mutex);
			if (m_CreateTask.Running) {
				show = true;
				title = IDX_TR("launcher.progress.creating_project");
				stage = m_CreateTask.Stage.empty()
					? IDX_TR("launcher.progress.working")
					: m_CreateTask.Stage;
				progress = m_CreateTask.Progress;
			}
		}

		// ── Asset library download ─────────────────────────────────
		if (!show) {
			AssetLibraryStage assetStage;
			std::string displayName;
			bool running;
			float assetProgress;
			{
				std::scoped_lock lock(m_AssetLibraryTask.Mutex);
				assetStage = m_AssetLibraryTask.Stage;
				displayName = m_AssetLibraryTask.DisplayName;
				running = m_AssetLibraryTask.Running;
				assetProgress = m_AssetLibraryTask.Progress;
			}
			if (running) {
				show = true;
				title = IDX_TR("launcher.progress.downloading_project");
				stage = AssetLibraryStageText(assetStage, displayName);
				if (stage.empty()) stage = IDX_TR("launcher.progress.working");
				progress = assetProgress;
			}
		}

		if (show) {
			Win32BuildProgressWindow::Show(title);
			Win32BuildProgressWindow::Update(progress, stage);
		}
		else if (Win32BuildProgressWindow::IsVisible()) {
			Win32BuildProgressWindow::Hide();
		}
	}

	void LauncherLayer::OpenProjectInExplorer(const LauncherProjectEntry& entry) {
		std::error_code ec;
		if (!std::filesystem::exists(entry.Path, ec) || ec) {
			ShowError(ec ? "Project folder could not be checked: " + ec.message() : "Project folder not found");
			return;
		}

#ifdef IDX_PLATFORM_WINDOWS
		const std::wstring projectPath = Utf8ToWide(entry.Path);
		if (projectPath.empty() && !entry.Path.empty()) {
			ShowError("Project path is not valid UTF-8");
			return;
		}

		const HINSTANCE result = ShellExecuteW(nullptr, L"open", projectPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		const INT_PTR resultCode = reinterpret_cast<INT_PTR>(result);
		if (resultCode <= 32) {
			ShowError("Failed to open project in Explorer (code " + std::to_string(resultCode) + ")");
		}
#elif defined(IDX_PLATFORM_LINUX)
		if (!Process::LaunchDetached({ "xdg-open", entry.Path })) {
			ShowError("Failed to open project in file manager");
		}
#else
		ShowError("Open in Explorer is not supported on this platform");
#endif
	}

	bool LauncherLayer::DeleteProjectFromDisk(const LauncherProjectEntry& entry) {
		// Both m_DeleteError (shown inline in modal) and ShowError (generic dialog) are set so the message appears in both places.
		auto fail = [this](std::string message) -> bool {
			m_DeleteError = std::move(message);
			ShowError(m_DeleteError);
			return false;
		};

		m_DeleteError.clear();

		std::error_code ec;
		std::filesystem::path projectPath = std::filesystem::weakly_canonical(entry.Path, ec);
		if (ec) {
			ec.clear();
			projectPath = std::filesystem::absolute(entry.Path, ec);
			if (ec) {
				return fail("Project folder could not be resolved: " + ec.message());
			}
		}
		projectPath = projectPath.lexically_normal();

		if (!IsSafeProjectDeleteRoot(projectPath)) {
			return fail("Refusing to delete an unsafe project folder path.");
		}

		const bool exists = std::filesystem::exists(projectPath, ec);
		if (ec) {
			return fail("Project folder could not be checked: " + ec.message());
		}
		if (!exists) {
			m_Registry.RemoveProject(entry.Path);
			m_Registry.Save();
			m_ProjectSizeTasks.erase(entry.Path);
			return true;
		}

		const bool isDirectory = std::filesystem::is_directory(projectPath, ec);
		if (ec) {
			return fail("Project folder could not be checked: " + ec.message());
		}
		if (!isDirectory) {
			return fail("Project path is not a folder. Use Remove from List instead.");
		}

		if (!IndexProject::Validate(projectPath.string())) {
			return fail("Folder no longer looks like a Index project. Use Remove from List instead.");
		}

		std::filesystem::remove_all(projectPath, ec);
		if (ec) {
			return fail("Failed to delete project folder: " + ec.message());
		}

		m_Registry.RemoveProject(entry.Path);
		m_Registry.Save();
		m_ProjectSizeTasks.erase(entry.Path);

		IDX_INFO_TAG("Launcher", "Deleted project '{}' at {}", entry.Name, projectPath.string());
		return true;
	}

	// ── Browse for Existing Project ─────────────────────────────────

	void LauncherLayer::BrowseForExistingProject() {
#ifdef IDX_PLATFORM_WINDOWS
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

		IFileOpenDialog* dialog = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
			IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog));

		if (SUCCEEDED(hr)) {
			DWORD options;
			dialog->GetOptions(&options);
			dialog->SetOptions(options | FOS_PICKFOLDERS);
			dialog->SetTitle(L"Select Index Project Folder");

			hr = dialog->Show(nullptr);
			if (SUCCEEDED(hr)) {
				IShellItem* result = nullptr;
				dialog->GetResult(&result);
				if (result) {
					PWSTR widePath = nullptr;
					result->GetDisplayName(SIGDN_FILESYSPATH, &widePath);
					if (widePath) {
						int len = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
						std::string path(len - 1, '\0');
						WideCharToMultiByte(CP_UTF8, 0, widePath, -1, path.data(), len, nullptr, nullptr);
						CoTaskMemFree(widePath);

						if (IndexProject::Validate(path)) {
							auto project = IndexProject::Load(path);
							m_Registry.AddProject(project.Name, project.RootDirectory);
							m_Registry.Save();
						}
						else {
							ShowError("Not a valid Index project (missing project.json or Assets/)");
						}
					}
					result->Release();
				}
			}
			dialog->Release();
		}

		CoUninitialize();
#endif
	}

	// ── Sort / Refresh / Settings ───────────────────────────────────

	std::vector<const LauncherProjectEntry*> LauncherLayer::GetSortedProjectsView() const {
		const auto& projects = m_Registry.GetProjects();
		std::vector<const LauncherProjectEntry*> view;
		view.reserve(projects.size());
		for (const auto& entry : projects) view.push_back(&entry);

		auto getCreatedAt = [&](const LauncherProjectEntry* e) -> std::int64_t {
			auto it = m_CreatedAtCache.find(e->Path);
			if (it != m_CreatedAtCache.end()) return it->second;
			std::error_code ec;
			std::int64_t ts = 0;
			std::filesystem::path p(e->Path);
			auto ftime = std::filesystem::last_write_time(p, ec);
			if (!ec) {
				ts = std::chrono::duration_cast<std::chrono::seconds>(
					ftime.time_since_epoch()).count();
			}
			m_CreatedAtCache.emplace(e->Path, ts);
			return ts;
		};

		switch (m_SortMode) {
			case SortMode::LastOpened:
				std::sort(view.begin(), view.end(),
					[](const auto* a, const auto* b) { return a->LastOpened > b->LastOpened; });
				break;
			case SortMode::Name:
				std::sort(view.begin(), view.end(),
					[](const auto* a, const auto* b) {
						return std::lexicographical_compare(
							a->Name.begin(), a->Name.end(),
							b->Name.begin(), b->Name.end(),
							[](unsigned char x, unsigned char y) {
								return std::tolower(x) < std::tolower(y);
							});
					});
				break;
			case SortMode::CreatedAt:
				std::sort(view.begin(), view.end(),
					[&](const auto* a, const auto* b) { return getCreatedAt(a) > getCreatedAt(b); });
				break;
		}

		if (m_SortReverse) {
			std::reverse(view.begin(), view.end());
		}
		return view;
	}

	void LauncherLayer::RefreshProjectsList() {
		// Stop any running size workers so they don't write into the cache
		// after Load() pulls in a different project list (path-keyed map
		// makes this strictly additive, but stopping eliminates wasted IO).
		for (auto& [_, task] : m_ProjectSizeTasks) {
			if (task) task->Worker.request_stop();
		}
		m_ProjectSizeTasks.clear();
		m_CreatedAtCache.clear();
		m_EngineVersionCache.clear();

		m_Registry.Load();
		m_Registry.ValidateAll();
		m_Registry.Save();
	}

	std::string LauncherLayer::GetSettingsPath() {
		return Path::Combine(
			Path::GetSpecialFolderPath(SpecialFolder::LocalAppData),
			"Index", "launcher_settings.json");
	}

	const char* LauncherLayer::DirectoryNameConventionLabel(DirectoryNameConvention convention) {
		switch (convention) {
		case DirectoryNameConvention::None:           return "None";
		case DirectoryNameConvention::TitleCase:      return "TitleCase";
		case DirectoryNameConvention::TitleDashCase:  return "Title-Case";
		case DirectoryNameConvention::TitleCamelCase: return "titleCase";
		case DirectoryNameConvention::TitleSnakeCase: return "title_case";
		case DirectoryNameConvention::TitleKebabCase: return "title-case";
		}
		return "TitleCase";
	}

	std::string LauncherLayer::ApplyDirectoryNameConvention(std::string_view name,
		DirectoryNameConvention convention)
	{
		if (convention == DirectoryNameConvention::None) {
			return std::string(name);
		}

		std::vector<std::string> words;
		std::string current;
		char previous = '\0';
		for (char raw : name) {
			const unsigned char c = static_cast<unsigned char>(raw);
			if (!std::isalnum(c)) {
				if (!current.empty()) {
					words.push_back(current);
					current.clear();
				}
				previous = '\0';
				continue;
			}

			const bool splitCamel = !current.empty()
				&& std::islower(static_cast<unsigned char>(previous))
				&& std::isupper(c);
			if (splitCamel) {
				words.push_back(current);
				current.clear();
			}
			current.push_back(raw);
			previous = raw;
		}
		if (!current.empty()) {
			words.push_back(current);
		}

		auto titleWord = [](std::string word) {
			if (word.empty()) return word;
			for (char& ch : word) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
			return word;
		};
		auto lowerWord = [](std::string word) {
			for (char& ch : word) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			return word;
		};

		std::string result;
		const char separator =
			(convention == DirectoryNameConvention::TitleDashCase) ? '-' :
			(convention == DirectoryNameConvention::TitleSnakeCase) ? '_' :
			(convention == DirectoryNameConvention::TitleKebabCase) ? '-' : '\0';

		for (std::size_t i = 0; i < words.size(); ++i) {
			std::string word;
			if (convention == DirectoryNameConvention::TitleCamelCase) {
				word = (i == 0) ? lowerWord(words[i]) : titleWord(words[i]);
			}
			else if (convention == DirectoryNameConvention::TitleSnakeCase
				|| convention == DirectoryNameConvention::TitleKebabCase) {
				word = lowerWord(words[i]);
			}
			else {
				word = titleWord(words[i]);
			}

			if (!result.empty() && separator != '\0') {
				result.push_back(separator);
			}
			result += word;
		}

		return result;
	}

	void LauncherLayer::LoadLauncherSettings() {
		const std::string path = GetSettingsPath();
		if (!File::Exists(path)) return;

		Json::Value root;
		std::string parseError;
		const std::string text = File::ReadAllText(path);
		if (!Json::TryParse(text, root, &parseError) || !root.IsObject()) {
			IDX_CORE_WARN_TAG("Launcher", "Failed to parse '{}': {}", path, parseError);
			return;
		}

		if (const Json::Value* v = root.FindMember("defaultProjectsLocation")) {
			m_DefaultProjectsLocation = v->AsStringOr();
		}
		if (const Json::Value* v = root.FindMember("sortMode")) {
			int mode = v->AsIntOr(static_cast<int>(SortMode::LastOpened));
			if (mode >= 0 && mode <= static_cast<int>(SortMode::CreatedAt)) {
				m_SortMode = static_cast<SortMode>(mode);
			}
		}
		if (const Json::Value* v = root.FindMember("sortReverse")) {
			m_SortReverse = v->AsBoolOr(false);
		}
		if (const Json::Value* v = root.FindMember("fontScale")) {
			int scale = v->AsIntOr(static_cast<int>(FontScale::Auto));
			if (scale >= 0 && scale <= static_cast<int>(FontScale::P200)) {
				m_FontScale = static_cast<FontScale>(scale);
			}
		}
		if (const Json::Value* v = root.FindMember("themeSetting")) {
			int setting = v->AsIntOr(static_cast<int>(ThemeSetting::Auto));
			if (setting >= 0 && setting <= static_cast<int>(ThemeSetting::Light)) {
				m_ThemeSetting = static_cast<ThemeSetting>(setting);
			}
		}
		if (const Json::Value* v = root.FindMember("directoryNameConvention")) {
			int convention = v->AsIntOr(static_cast<int>(DirectoryNameConvention::TitleCase));
			if (convention >= 0 && convention <= static_cast<int>(DirectoryNameConvention::TitleKebabCase)) {
				m_DirectoryNameConvention = static_cast<DirectoryNameConvention>(convention);
			}
		}
		if (const Json::Value* v = root.FindMember("languageAuto")) {
			m_LanguageAuto = v->AsBoolOr(false);
		}
		if (const Json::Value* v = root.FindMember("assetLibraryTrustAck"); v && v->IsArray()) {
			for (const auto& item : v->GetArray()) {
				if (item.IsString()) m_AssetLibraryTrustAck[item.AsStringOr("")] = true;
			}
		}
	}

	void LauncherLayer::SaveLauncherSettings() const {
		const std::string path = GetSettingsPath();

		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

		Json::Value root = Json::Value::MakeObject();
		root.AddMember("defaultProjectsLocation", m_DefaultProjectsLocation);
		root.AddMember("sortMode", Json::Value(static_cast<int>(m_SortMode)));
		root.AddMember("sortReverse", Json::Value(m_SortReverse));
		root.AddMember("fontScale", Json::Value(static_cast<int>(m_FontScale)));
		root.AddMember("themeSetting", Json::Value(static_cast<int>(m_ThemeSetting)));
		root.AddMember("directoryNameConvention", Json::Value(static_cast<int>(m_DirectoryNameConvention)));
		root.AddMember("languageAuto", Json::Value(m_LanguageAuto));
		{
			Json::Value ack = Json::Value::MakeArray();
			for (const auto& [url, acked] : m_AssetLibraryTrustAck) {
				if (acked) ack.Append(Json::Value(url));
			}
			root.AddMember("assetLibraryTrustAck", std::move(ack));
		}
		if (!File::WriteAllText(path, Json::Stringify(root, true))) {
			IDX_CORE_WARN_TAG("Launcher", "Failed to write launcher settings to '{}'", path);
		}
	}

	void LauncherLayer::ShowError(std::string message) {
		m_ErrorMessage = std::move(message);
		m_OpenErrorPopup = true;
	}

	void LauncherLayer::RenderErrorPopup() {
		constexpr const char* k_ErrorId = "###LauncherError";

		if (m_OpenErrorPopup) {
			ImGui::OpenPopup(k_ErrorId);
			m_OpenErrorPopup = false;
		}

		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);

		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		const std::string title = IDX_TR("launcher.error.title") + k_ErrorId;
		if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return;
		}
		ImGui::TextWrapped("%s", m_ErrorMessage.c_str());
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// Centre the OK button so the dialog feels balanced regardless of
		// message length. Width matches the Save/Cancel buttons elsewhere
		// in the launcher for visual consistency.
		const float buttonW = 100.0f;
		ImGui::SetCursorPosX((ImGui::GetWindowWidth() - buttonW) * 0.5f);
		if (ImGui::Button(IDX_TR("launcher.error.ok").c_str(), ImVec2(buttonW, 0))) {
			m_ErrorMessage.clear();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void LauncherLayer::RenderSettingsPopup() {
		constexpr const char* k_SettingsId = "###LauncherSettings";

		if (m_OpenSettingsPopup) {
			ImGui::OpenPopup(k_SettingsId);
			m_OpenSettingsPopup = false;
		}

		{
			const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
			ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		}
		ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		const std::string settingsTitle = IDX_TR("launcher.settings.title") + k_SettingsId;
		if (!ImGui::BeginPopupModal(settingsTitle.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return;
		}
		const std::string effectiveDefault = m_DefaultProjectsLocation.empty()
			? IndexProject::GetDefaultProjectsDir()
			: m_DefaultProjectsLocation;

		ImGui::TextUnformatted(IDX_TR("launcher.settings.default_location").c_str());
		ImGui::TextDisabled("%s", IDX_TR("launcher.settings.default_location_subtitle").c_str());

		const std::string currentLine = Localization::Format("launcher.settings.current", effectiveDefault)
			+ (m_DefaultProjectsLocation.empty() ? IDX_TR("launcher.settings.engine_default_suffix") : std::string{});
		ImGui::TextDisabled("%s", currentLine.c_str());
		ImGui::Spacing();

		static char s_LocationBuffer[1024]{};
		if (ImGui::IsWindowAppearing()) {
			std::snprintf(s_LocationBuffer, sizeof(s_LocationBuffer), "%s",
				effectiveDefault.c_str());
		}
		ImGui::SetNextItemWidth(-110.0f);
		ImGui::InputText("##DefaultLocation", s_LocationBuffer, sizeof(s_LocationBuffer));
		ImGui::SameLine();
		if (ImGui::Button(IDX_TR("launcher.settings.browse").c_str(), ImVec2(100, 0))) {
			BrowseForDefaultProjectsLocation();
			std::snprintf(s_LocationBuffer, sizeof(s_LocationBuffer), "%s",
				m_DefaultProjectsLocation.c_str());
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextUnformatted(IDX_TR("launcher.settings.directory_name_convention").c_str());
		{
			static constexpr DirectoryNameConvention k_Entries[] = {
				DirectoryNameConvention::None,
				DirectoryNameConvention::TitleCase,
				DirectoryNameConvention::TitleDashCase,
				DirectoryNameConvention::TitleCamelCase,
				DirectoryNameConvention::TitleSnakeCase,
				DirectoryNameConvention::TitleKebabCase,
			};

			const char* preview = DirectoryNameConventionLabel(m_DirectoryNameConvention);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::BeginCombo("##LauncherDirectoryNameConventionCombo", preview)) {
				for (DirectoryNameConvention convention : k_Entries) {
					const bool selected = convention == m_DirectoryNameConvention;
					const char* label = DirectoryNameConventionLabel(convention);
					if (ImGui::Selectable(label, selected)) {
						m_DirectoryNameConvention = convention;
						SaveLauncherSettings();
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextUnformatted(IDX_TR("launcher.settings.language").c_str());
		const auto& languages = Localization::GetAvailableLanguages();
		const std::string& currentCode = Localization::GetCurrentLanguage();
		const std::string notInstalledSuffix = IDX_TR("launcher.settings.language.not_installed_suffix");

		const std::string sysCode = Localization::GetSystemLanguage();
		std::string sysDisplay = sysCode;
		for (const auto& lang : languages) {
			if (lang.Code == sysCode) { sysDisplay = lang.DisplayName; break; }
		}
		const std::string autoLabel = Localization::Format("launcher.settings.language.auto_format", sysDisplay);

		std::string previewName;
		if (m_LanguageAuto) {
			previewName = autoLabel;
		}
		else {
			for (const auto& lang : languages) {
				if (lang.Code == currentCode) { previewName = lang.DisplayName; break; }
			}
		}

		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo("##LauncherLanguageCombo", previewName.c_str())) {
			// Auto entry always at the top — matches the screenshot the user
			// referenced and lets a returning user re-engage system-language
			// tracking without hunting for a checkbox.
			if (ImGui::Selectable(autoLabel.c_str(), m_LanguageAuto)) {
				m_LanguageAuto = true;
				Localization::SetLanguage(sysCode);
				SaveLauncherSettings();
			}
			if (m_LanguageAuto) ImGui::SetItemDefaultFocus();

			ImGui::Separator();

			for (size_t i = 0; i < languages.size(); ++i) {
				const bool selected = !m_LanguageAuto && (languages[i].Code == currentCode);
				std::string label = languages[i].DisplayName;
				if (languages[i].Status != Localization::LanguageStatus::Installed) {
					label += notInstalledSuffix;
				}
				if (ImGui::Selectable(label.c_str(), selected)) {
					m_LanguageAuto = false;
					Localization::SetLanguage(languages[i].Code);
					SaveLauncherSettings();
				}
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		if (const auto download = Localization::GetActiveDownload()) {
			ImGui::Spacing();
			if (download->Failed) {
				const std::string msg = Localization::Format("launcher.settings.language.download_failed", download->Error);
				ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", msg.c_str());
			}
			else if (download->RestartRequired) {
				ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "%s",
					IDX_TR("launcher.settings.language.restart_required").c_str());
				if (ImGui::Button(IDX_TR("launcher.settings.language.restart_now").c_str())) {
					RestartLauncherProcess();
				}
			}
			else {
				const std::string msg = Localization::Format("launcher.settings.language.downloading", download->Code);
				ImGui::TextDisabled("%s", msg.c_str());
				ImGui::ProgressBar(download->Progress, ImVec2(-1.0f, 0));
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextUnformatted(IDX_TR("launcher.settings.theme").c_str());
		{
			struct ThemeEntry {
				ThemeSetting Value;
				const char* LabelKey;
			};
			static constexpr ThemeEntry k_Entries[] = {
				{ ThemeSetting::Auto,  nullptr },
				{ ThemeSetting::Dark,  "launcher.settings.theme.dark" },
				{ ThemeSetting::Light, "launcher.settings.theme.light" },
			};

			const std::string effectiveThemeName = IsEffectiveThemeDark()
				? IDX_TR("launcher.settings.theme.dark")
				: IDX_TR("launcher.settings.theme.light");

			auto labelFor = [&](const ThemeEntry& e) -> std::string {
				if (e.Value == ThemeSetting::Auto) {
					return Localization::Format("launcher.settings.theme.auto_format", effectiveThemeName);
				}
				return IDX_TR(e.LabelKey);
			};

			std::string preview;
			for (const auto& e : k_Entries) {
				if (e.Value == m_ThemeSetting) {
					preview = labelFor(e);
					break;
				}
			}

			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::BeginCombo("##LauncherThemeCombo", preview.c_str())) {
				for (const auto& e : k_Entries) {
					const bool selected = (e.Value == m_ThemeSetting);
					const std::string label = labelFor(e);
					if (ImGui::Selectable(label.c_str(), selected)) {
						m_ThemeSetting = e.Value;
						m_LastAppliedDarkTheme.reset();
						SaveLauncherSettings();
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::TextUnformatted(IDX_TR("launcher.settings.font_scale").c_str());
		{
			struct FontScaleEntry {
				FontScale Value;
				const char* Label;  // nullptr → use translated "Auto (xxx%)"
			};
			static constexpr FontScaleEntry k_Entries[] = {
				{ FontScale::Auto, nullptr },
				{ FontScale::P75,  "75%"   },
				{ FontScale::P100, "100%"  },
				{ FontScale::P125, "125%"  },
				{ FontScale::P150, "150%"  },
				{ FontScale::P175, "175%"  },
				{ FontScale::P200, "200%"  },
			};

			// Auto always resolves to 100% today; format the {0} so future
			// DPI-aware behaviour can change the displayed percentage without
			// touching the translation files.
			auto labelFor = [](const FontScaleEntry& e) -> std::string {
				if (e.Value == FontScale::Auto) {
					return Localization::Format("launcher.settings.font_scale.auto_format", 100);
				}
				return std::string(e.Label);
			};

			std::string preview;
			for (const auto& e : k_Entries) {
				if (e.Value == m_FontScale) {
					preview = labelFor(e);
					break;
				}
			}

			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::BeginCombo("##LauncherFontScaleCombo", preview.c_str())) {
				for (const auto& e : k_Entries) {
					const bool selected = (e.Value == m_FontScale);
					const std::string label = labelFor(e);
					if (ImGui::Selectable(label.c_str(), selected)) {
						m_FontScale = e.Value;
						SaveLauncherSettings();
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		if (ImGui::Button(IDX_TR("launcher.settings.save").c_str(), ImVec2(100, 0))) {
			// Empty string = "follow engine default"; don't persist verbatim engine default as an override.
			const std::string typed(s_LocationBuffer);
			if (typed == IndexProject::GetDefaultProjectsDir()) {
				m_DefaultProjectsLocation.clear();
			}
			else {
				m_DefaultProjectsLocation = typed;
			}
			SaveLauncherSettings();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(IDX_TR("launcher.settings.cancel").c_str(), ImVec2(100, 0))) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	static std::string FormatLocalDateTime(std::int64_t secondsSinceEpoch) {
		if (secondsSinceEpoch <= 0) return "unknown";
		std::time_t t = static_cast<std::time_t>(secondsSinceEpoch);
		std::tm tm{};
#ifdef IDX_PLATFORM_WINDOWS
		if (localtime_s(&tm, &t) != 0) return "unknown";
#else
		if (!localtime_r(&t, &tm)) return "unknown";
#endif
		std::ostringstream ss;
		ss << std::put_time(&tm, "%Y-%m-%d %H:%M");
		return ss.str();
	}

	void LauncherLayer::RenderProjectInfoPopup() {
		constexpr const char* k_InfoId = "###LauncherProjectInfo";

		if (m_OpenInfoPopup) {
			ImGui::OpenPopup(k_InfoId);
			m_OpenInfoPopup = false;
		}

		{
			const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
			ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		}
		ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		const std::string infoTitle = IDX_TR("launcher.info.title") + k_InfoId;
		if (!ImGui::BeginPopupModal(infoTitle.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return;
		}

		if (!m_PendingInfoProject.has_value()) {
			// Nothing to show — close defensively so the modal can't get stuck.
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		const LauncherProjectEntry& entry = *m_PendingInfoProject;

		auto versionIt = m_EngineVersionCache.find(entry.Path);
		if (versionIt == m_EngineVersionCache.end()) {
			std::string version;
			if (IndexProject::Validate(entry.Path)) {
				auto project = IndexProject::Load(entry.Path);
				version = project.EngineVersion;
			}
			// Cache an empty sentinel rather than the translated "unknown"
			// string — that way hot-reloading the language picks up the new
			// translation on next frame without invalidating the cache.
			versionIt = m_EngineVersionCache.emplace(entry.Path, std::move(version)).first;
		}
		const std::string& engineVersionCached = versionIt->second;
		const std::string engineVersion = engineVersionCached.empty()
			? IDX_TR("launcher.info.unknown")
			: engineVersionCached;

		auto createdIt = m_CreatedAtCache.find(entry.Path);
		if (createdIt == m_CreatedAtCache.end()) {
			std::error_code ec;
			std::int64_t ts = 0;
			std::filesystem::path p(entry.Path);
			auto ftime = std::filesystem::last_write_time(p, ec);
			if (!ec) {
				ts = std::chrono::duration_cast<std::chrono::seconds>(
					ftime.time_since_epoch()).count();
			}
			createdIt = m_CreatedAtCache.emplace(entry.Path, ts).first;
		}
		const std::string createdStr = FormatLocalDateTime(createdIt->second);

		const std::string sizeStr = GetProjectSizeDisplayText(entry);
		const std::string relativeLastOpened = FormatRelativeTime(entry.LastOpened);

		// Header line: project name as the modal's title-ish content.
		ImGui::TextUnformatted(entry.Name.c_str());
		ImGui::Separator();
		ImGui::Spacing();

		// Two-column table: left column auto-sized to the longest label, right
		// column stretches to fill. Horizontal inner borders give visual
		// separation between rows without making it look like a heavy form.
		if (ImGui::BeginTable("##ProjectInfoTable", 2,
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH)) {
			ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

			auto labelValueRow = [](const char* label, const char* value) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(label);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextWrapped("%s", value && *value ? value : "—");
			};

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(IDX_TR("launcher.info.path").c_str());
			ImGui::TableSetColumnIndex(1);
			{
				static char s_PathBuffer[1024];
				std::snprintf(s_PathBuffer, sizeof(s_PathBuffer), "%s", entry.Path.c_str());
				const float buttonWidth = 70.0f;
				const float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
				const float pathInputWidth = std::max(120.0f,
					ImGui::GetContentRegionAvail().x - buttonWidth - itemSpacing);
				ImGui::SetNextItemWidth(pathInputWidth);
				ImGui::InputText("##InfoPath", s_PathBuffer, sizeof(s_PathBuffer),
					ImGuiInputTextFlags_ReadOnly);
				ImGui::SameLine();
				if (ImGui::Button(IDX_TR("launcher.info.open").c_str(), ImVec2(buttonWidth, 0))) {
					OpenProjectInExplorer(entry);
				}
			}

			labelValueRow(IDX_TR("launcher.info.created").c_str(), createdStr.c_str());

			// Last opened: show the relative form ("3h ago") plus the raw ISO
			// timestamp in parens so power users can see the exact moment.
			std::string lastOpenedCombined;
			if (!entry.LastOpened.empty()) {
				if (!relativeLastOpened.empty()) {
					lastOpenedCombined = relativeLastOpened + "  (" + entry.LastOpened + ")";
				} else {
					lastOpenedCombined = entry.LastOpened;
				}
			}
			labelValueRow(IDX_TR("launcher.info.last_opened").c_str(),
				lastOpenedCombined.empty() ? IDX_TR("launcher.info.never").c_str() : lastOpenedCombined.c_str());

			labelValueRow(IDX_TR("launcher.info.size").c_str(), sizeStr.c_str());
			labelValueRow(IDX_TR("launcher.info.engine_version").c_str(), engineVersion.c_str());

			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		if (ImGui::Button(IDX_TR("launcher.info.close").c_str(), ImVec2(100, 0))) {
			m_PendingInfoProject.reset();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void LauncherLayer::BrowseForDefaultProjectsLocation() {
#ifdef IDX_PLATFORM_WINDOWS
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

		IFileOpenDialog* dialog = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
			IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog));

		if (SUCCEEDED(hr)) {
			DWORD options;
			dialog->GetOptions(&options);
			dialog->SetOptions(options | FOS_PICKFOLDERS);
			dialog->SetTitle(L"Default projects location");

			if (SUCCEEDED(dialog->Show(nullptr))) {
				IShellItem* result = nullptr;
				dialog->GetResult(&result);
				if (result) {
					PWSTR widePath = nullptr;
					result->GetDisplayName(SIGDN_FILESYSPATH, &widePath);
					if (widePath) {
						const int len = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
						if (len > 1) {
							std::string path(static_cast<size_t>(len - 1), '\0');
							WideCharToMultiByte(CP_UTF8, 0, widePath, -1, path.data(), len, nullptr, nullptr);
							m_DefaultProjectsLocation = std::move(path);
						}
						CoTaskMemFree(widePath);
					}
					result->Release();
				}
			}
			dialog->Release();
		}

		CoUninitialize();
#endif
	}

	// ── Asset Library ─────────────────────────────────────────────────

	namespace {

		constexpr const char* k_DefaultAssetLibraryUrl =
			"https://raw.githubusercontent.com/Ben-Scr/Index-AssetLibrary/main/index.json";

		std::string ResolveAssetLibraryUrl() {
			if (const char* env = std::getenv("INDEX_ASSET_LIBRARY_URL"); env && *env) {
				return env;
			}
			return k_DefaultAssetLibraryUrl;
		}

		// Whitelist limits to GitHub: prevents a compromised mirror redirecting downloads.
		bool IsTrustedArchiveHost(std::string_view url) {
			constexpr std::string_view k_Trusted[] = {
				"https://github.com/",
				"https://objects.githubusercontent.com/",
				"https://raw.githubusercontent.com/",
				"https://codeload.github.com/",
			};
			for (auto prefix : k_Trusted) {
				if (url.size() >= prefix.size()
					&& std::string_view(url.data(), prefix.size()) == prefix) {
					return true;
				}
			}
			return false;
		}

		std::vector<std::string> JsonStringArray(const Json::Value* v) {
			std::vector<std::string> out;
			if (!v || !v->IsArray()) return out;
			for (const auto& item : v->GetArray()) {
				if (item.IsString()) out.push_back(item.AsStringOr(""));
			}
			return out;
		}

		LauncherLayer::AssetLibraryEntry ParseLibraryEntry(const Json::Value& obj) {
			LauncherLayer::AssetLibraryEntry e;
			auto str = [&](const char* k) -> std::string {
				const Json::Value* v = obj.FindMember(k);
				return v ? v->AsStringOr("") : "";
			};
			e.Id = str("id");
			e.Name = str("name");
			e.Author = str("author");
			e.ShortDescription = str("shortDescription");
			e.Description = str("description");
			e.Tags = JsonStringArray(obj.FindMember("tags"));
			e.Category = str("category");
			e.Version = str("version");
			e.License = str("license");
			e.ThumbnailUrl = str("thumbnail");
			e.ScreenshotUrls = JsonStringArray(obj.FindMember("screenshots"));
			if (const Json::Value* archive = obj.FindMember("archive"); archive && archive->IsObject()) {
				if (auto* v = archive->FindMember("url"); v) e.ArchiveUrl = v->AsStringOr("");
				if (auto* v = archive->FindMember("sha256"); v) e.ArchiveSha256 = Hash::ToLowerHex(v->AsStringOr(""));
				if (auto* v = archive->FindMember("sizeBytes"); v) e.ArchiveSizeBytes = v->AsUInt64Or(0);
				if (auto* v = archive->FindMember("rootInZip"); v) e.ArchiveRootInZip = v->AsStringOr("");
			}
			if (const Json::Value* engine = obj.FindMember("engine"); engine && engine->IsObject()) {
				if (auto* v = engine->FindMember("minVersion"); v) e.EngineMinVersion = v->AsStringOr("");
				if (auto* v = engine->FindMember("maxVersion"); v) e.EngineMaxVersion = v->AsStringOr("");
			}
			e.RequiredPackages = JsonStringArray(obj.FindMember("requiredPackages"));
			e.Homepage = str("homepage");
			e.SourceUrl = str("sourceUrl");
			return e;
		}

		// Accepts flat archives (project.json at root) and single-subdir archives; rejects anything else.
		std::filesystem::path PickExtractedProjectRoot(const std::filesystem::path& stagingDir) {
			std::error_code ec;
			if (std::filesystem::exists(stagingDir / "project.json", ec)) {
				return stagingDir;
			}
			std::filesystem::path onlySubdir;
			int dirCount = 0;
			int topFiles = 0;
			for (auto& entry : std::filesystem::directory_iterator(stagingDir, ec)) {
				if (ec) break;
				if (entry.is_directory(ec)) {
					onlySubdir = entry.path();
					++dirCount;
				}
				else if (entry.is_regular_file(ec)) {
					++topFiles;
				}
			}
			if (dirCount == 1 && topFiles == 0) {
				if (std::filesystem::exists(onlySubdir / "project.json", ec)) {
					return onlySubdir;
				}
			}
			return {};
		}

	} // anonymous namespace

	std::string LauncherLayer::AssetLibraryStageText(AssetLibraryStage stage,
		std::string_view displayName) const
	{
		switch (stage) {
		case AssetLibraryStage::Idle:
			return "";
		case AssetLibraryStage::FetchingManifest:
			return IDX_TR("launcher.asset_library.progress.fetching_manifest");
		case AssetLibraryStage::Downloading:
			return std::vformat(
				IDX_TR("launcher.asset_library.progress.downloading"),
				std::make_format_args(displayName));
		case AssetLibraryStage::Verifying:
			return IDX_TR("launcher.asset_library.progress.verifying");
		case AssetLibraryStage::Extracting:
			return IDX_TR("launcher.asset_library.progress.extracting");
		case AssetLibraryStage::Importing:
			return IDX_TR("launcher.asset_library.progress.importing");
		case AssetLibraryStage::Done:
			return IDX_TR("launcher.asset_library.progress.done");
		case AssetLibraryStage::Error:
			return IDX_TR("launcher.asset_library.progress.done"); // overlay handles error
		}
		return "";
	}

	const LauncherLayer::AssetLibraryEntry* LauncherLayer::FindAssetLibraryEntry(std::string_view id) const {
		for (const auto& entry : m_AssetLibrary.Entries) {
			if (entry.Id == id) return &entry;
		}
		return nullptr;
	}

	void LauncherLayer::StartFetchAssetLibraryIndex() {
		{
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			if (m_AssetLibrary.FetchInFlight) return;
			m_AssetLibrary.FetchInFlight = true;
			m_AssetLibrary.FetchError.clear();
		}

		// If a previous worker is still alive (download or fetch), join it
		// first. Single-slot pipeline by design.
		if (m_AssetLibraryTask.Worker.joinable()) m_AssetLibraryTask.Worker.join();

		m_AssetLibraryTask.Worker = std::thread([this]() {
			std::string error;
			std::vector<AssetLibraryEntry> parsed;
			int schemaVersion = 0;
			std::string generatedAt;

			const std::filesystem::path tool = PackageTool::ResolveExecutable();
			if (tool.empty()) {
				error = "Index-PackageTool not found";
			}
			else {
				std::vector<std::string> cmd;
				if (tool.extension() == ".dll") {
					cmd.push_back("dotnet");
					cmd.push_back(tool.string());
				}
				else {
					cmd.push_back(tool.string());
				}
				cmd.push_back("github-index");
				cmd.push_back(ResolveAssetLibraryUrl());

				// github-index has the 15s top-level timeout still, but for a
				// small JSON manifest that's fine.
				const Process::Result result = Process::Run(cmd, {}, std::chrono::seconds(30));
				if (!result.Succeeded()) {
					error = "Couldn't fetch library index (exit code "
						+ std::to_string(result.ExitCode) + ")";
				}
				else {
					std::string parseError;
					Json::Value root;
					if (!Json::TryParse(result.Output, root, &parseError) || !root.IsObject()) {
						error = "Index is not valid JSON: " + parseError;
					}
					else {
						if (auto* v = root.FindMember("schemaVersion"); v) schemaVersion = v->AsIntOr(0);
						if (auto* v = root.FindMember("generatedAt"); v) generatedAt = v->AsStringOr("");

						if (schemaVersion > 1) {
							error = "Index schema is newer than this launcher supports";
						}
						else if (auto* entries = root.FindMember("entries"); entries && entries->IsArray()) {
							for (const auto& entryVal : entries->GetArray()) {
								if (!entryVal.IsObject()) continue;
								AssetLibraryEntry e = ParseLibraryEntry(entryVal);
								if (e.Id.empty() || e.Name.empty() || e.ArchiveUrl.empty()) continue;
								parsed.push_back(std::move(e));
							}
						}
					}
				}
			}

			{
				std::scoped_lock lock(m_AssetLibraryTask.Mutex);
				m_AssetLibrary.SchemaVersion = schemaVersion;
				m_AssetLibrary.GeneratedAt = std::move(generatedAt);
				m_AssetLibrary.Entries = std::move(parsed);
				m_AssetLibrary.Loaded = error.empty();
				m_AssetLibrary.FetchInFlight = false;
				m_AssetLibrary.FetchError = error;
				m_AssetLibrary.LastRefreshedAt = std::chrono::steady_clock::now();
			}
			if (!error.empty()) {
				IDX_CORE_WARN_TAG("AssetLibrary", "Fetch failed: {}", error);
			}
			else {
				IDX_CORE_INFO_TAG("AssetLibrary", "Loaded {} entries", m_AssetLibrary.Entries.size());
			}
		});
	}

	void LauncherLayer::StartAssetLibraryDownload(const AssetLibraryEntry& entry) {
		// Bypass the GitHub host whitelist when an explicit override URL is
		// set — that's a dev/QA escape hatch (e.g. python -m http.server on
		// localhost). Default-config installs still get the strict check.
		const bool urlOverride = std::getenv("INDEX_ASSET_LIBRARY_URL") != nullptr;
		if (!urlOverride && !IsTrustedArchiveHost(entry.ArchiveUrl)) {
			ShowError(IDX_TR("launcher.asset_library.error.untrusted_host"));
			return;
		}

		// Trust gate: show the one-shot disclosure modal the first time the
		// user downloads from this source URL on this machine. The download
		// kicks off only after acknowledgement.
		const std::string sourceKey = ResolveAssetLibraryUrl();
		auto it = m_AssetLibraryTrustAck.find(sourceKey);
		if (it == m_AssetLibraryTrustAck.end() || !it->second) {
			m_PendingTrustedDownload = entry;
			m_OpenAssetLibraryTrustPopup = true;
			return;
		}

		{
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			if (m_AssetLibraryTask.Running) return; // already downloading something
			m_AssetLibraryTask.EntryId = entry.Id;
			m_AssetLibraryTask.DisplayName = entry.Name;
			m_AssetLibraryTask.Stage = AssetLibraryStage::FetchingManifest;
			m_AssetLibraryTask.Progress = 0.0f;
			m_AssetLibraryTask.Error.clear();
			m_AssetLibraryTask.TargetProjectPath.clear();
			m_AssetLibraryTask.Running = true;
			m_AssetLibraryTask.Finished = false;
			m_AssetLibraryTask.Success = false;
		}

		// Join previous worker first (could be a finished fetch).
		if (m_AssetLibraryTask.Worker.joinable()) m_AssetLibraryTask.Worker.join();

		AssetLibraryEntry entryCopy = entry; // capture by value
		m_AssetLibraryTask.Worker = std::thread(
			&LauncherLayer::AssetLibraryDownloadWorkerBody, this, std::move(entryCopy));
	}

	void LauncherLayer::AssetLibraryDownloadWorkerBody(AssetLibraryEntry entry) {
		auto setStage = [&](AssetLibraryStage stage, float progress) {
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			m_AssetLibraryTask.Stage = stage;
			m_AssetLibraryTask.Progress = progress;
		};
		auto fail = [&](std::string error) {
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			m_AssetLibraryTask.Stage = AssetLibraryStage::Error;
			m_AssetLibraryTask.Error = std::move(error);
			m_AssetLibraryTask.Running = false;
			m_AssetLibraryTask.Finished = true;
			m_AssetLibraryTask.Success = false;
		};

		const std::filesystem::path tool = PackageTool::ResolveExecutable();
		if (tool.empty()) { fail("Index-PackageTool not found"); return; }

		// Engine version compatibility — refuse if we don't meet minVersion.
		if (!entry.EngineMinVersion.empty()
			&& !Version::IsAtLeast(IDX_VERSION, entry.EngineMinVersion))
		{
			fail("Requires Index " + entry.EngineMinVersion);
			return;
		}

		setStage(AssetLibraryStage::Downloading, 0.0f);

		// Stage download to <projects>/.asset-library-staging/<id>-<rand>.zip.tmp
		const std::string baseDir = m_DefaultProjectsLocation.empty()
			? IndexProject::GetDefaultProjectsDir()
			: m_DefaultProjectsLocation;
		std::error_code ec;
		const std::filesystem::path stagingRoot = std::filesystem::path(baseDir) / ".asset-library-staging";
		std::filesystem::create_directories(stagingRoot, ec);

		// Unique suffix lets two concurrent downloads of the same id coexist
		// even though the UI prevents it today — costs nothing to be defensive.
		const auto now = std::chrono::steady_clock::now().time_since_epoch();
		const std::string token = std::to_string(now.count());
		const std::filesystem::path tempZip = stagingRoot / (entry.Id + "-" + token + ".zip.tmp");
		const std::filesystem::path stagingDir = stagingRoot / (entry.Id + "-" + token);

		auto cleanup = [&]() {
			std::error_code _ec;
			std::filesystem::remove(tempZip, _ec);
			std::filesystem::remove_all(stagingDir, _ec);
		};

		std::vector<std::string> dlCmd;
		if (tool.extension() == ".dll") { dlCmd.push_back("dotnet"); dlCmd.push_back(tool.string()); }
		else { dlCmd.push_back(tool.string()); }
		dlCmd.push_back("github-download");
		dlCmd.push_back(entry.ArchiveUrl);
		dlCmd.push_back(tempZip.string());

		const Process::Result dlResult = Process::Run(dlCmd, {}, std::chrono::milliseconds(0));
		if (!dlResult.Succeeded()) {
			cleanup();
			fail("Download failed (exit code " + std::to_string(dlResult.ExitCode) + ")");
			return;
		}
		setStage(AssetLibraryStage::Verifying, 0.0f);

		if (!entry.ArchiveSha256.empty()) {
			std::string actual, hashError;
			if (!Hash::Sha256OfFile(tempZip, actual, hashError)) {
				cleanup();
				fail(hashError);
				return;
			}
			if (actual != entry.ArchiveSha256) {
				cleanup();
				fail(IDX_TR("launcher.asset_library.error.verification"));
				return;
			}
		}

		setStage(AssetLibraryStage::Extracting, 0.0f);
		std::filesystem::create_directories(stagingDir, ec);
		std::vector<std::string> unzipCmd;
		if (tool.extension() == ".dll") { unzipCmd.push_back("dotnet"); unzipCmd.push_back(tool.string()); }
		else { unzipCmd.push_back(tool.string()); }
		unzipCmd.push_back("unzip");
		unzipCmd.push_back(tempZip.string());
		unzipCmd.push_back(stagingDir.string());
		const Process::Result unzipResult = Process::Run(unzipCmd, {}, std::chrono::milliseconds(0));
		if (!unzipResult.Succeeded()) {
			cleanup();
			fail("Extraction failed (exit code " + std::to_string(unzipResult.ExitCode) + ")");
			return;
		}

		// Locate the project root inside the extracted dir.
		std::filesystem::path projectRoot = PickExtractedProjectRoot(stagingDir);
		if (projectRoot.empty()) {
			cleanup();
			fail(IDX_TR("launcher.asset_library.error.no_project_in_archive"));
			return;
		}

		setStage(AssetLibraryStage::Importing, 0.0f);

		// Move into final <projects>/<sanitized-name>/ with collision suffix.
		auto sanitize = [](std::string_view s) {
			std::string out;
			out.reserve(s.size());
			for (char c : s) {
				if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
					|| c == '"' || c == '<' || c == '>' || c == '|') {
					out.push_back('-');
				}
				else { out.push_back(c); }
			}
			return out;
		};
		std::filesystem::path finalDir = std::filesystem::path(baseDir) / sanitize(entry.Name);
		for (int suffix = 1; std::filesystem::exists(finalDir, ec); ++suffix) {
			finalDir = std::filesystem::path(baseDir) / (sanitize(entry.Name) + "-" + std::to_string(suffix));
			if (suffix > 999) { cleanup(); fail("Couldn't find a free project name"); return; }
		}

		std::filesystem::rename(projectRoot, finalDir, ec);
		if (ec) {
			cleanup();
			fail("Couldn't move project into place: " + ec.message());
			return;
		}
		// Drop the staging dir if anything left over (only the picked subdir
		// gets renamed; siblings — there shouldn't be any — are removed).
		std::filesystem::remove_all(stagingDir, ec);
		std::filesystem::remove(tempZip, ec);

		// Validate the project by re-loading it. Load returns by value and
		// silently populates partial data on a malformed file — we check
		// the Name field to detect the "manifest missing / unparseable" case.
		IndexProject loaded = IndexProject::Load(finalDir.string());
		if (loaded.Name.empty()) {
			fail("Project failed to load: project.json is missing or invalid");
			return;
		}

		const std::string finalName = loaded.Name.empty() ? entry.Name : loaded.Name;
		m_Registry.AddProject(finalName, finalDir.string());
		m_Registry.Save();

		{
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			m_AssetLibraryTask.Stage = AssetLibraryStage::Done;
			m_AssetLibraryTask.Progress = 1.0f;
			m_AssetLibraryTask.Running = false;
			m_AssetLibraryTask.Finished = true;
			m_AssetLibraryTask.Success = true;
			m_AssetLibraryTask.TargetProjectPath = finalDir.string();
		}
		IDX_CORE_INFO_TAG("AssetLibrary", "Imported '{}' to '{}'",
			finalName, finalDir.string());
	}

	void LauncherLayer::PollAssetLibraryTask() {
		bool finishedThisFrame = false;
		std::string targetPath;
		{
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			if (m_AssetLibraryTask.Finished && !m_AssetLibraryTask.Running
				&& m_AssetLibraryTask.Stage != AssetLibraryStage::Idle)
			{
				finishedThisFrame = m_AssetLibraryTask.Success;
				targetPath = m_AssetLibraryTask.TargetProjectPath;
			}
		}
		if (finishedThisFrame && !targetPath.empty()) {
			// Refresh the registry view so the new project shows up under
			// My Projects without restarting. Don't auto-switch tabs. Completion
			// is surfaced via a modal (RenderAssetLibraryDoneModal), so this
			// one-shot also raises its open flag.
			RefreshProjectsList();
			m_SelectedProjectPath = targetPath;
			m_OpenAssetLibraryDonePopup = true;
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			m_AssetLibraryTask.TargetProjectPath.clear(); // one-shot
		}
	}

	void LauncherLayer::RenderAssetLibraryTab() {
		// Status / refresh strip.
		bool fetchInFlight = false;
		bool indexLoaded = false;
		std::string fetchError;
		size_t entryCount = 0;
		{
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			fetchInFlight = m_AssetLibrary.FetchInFlight;
			indexLoaded = m_AssetLibrary.Loaded;
			fetchError = m_AssetLibrary.FetchError;
			entryCount = m_AssetLibrary.Entries.size();
		}

		const float strip_h = ImGui::GetFrameHeight();
		ImGui::BeginGroup();

		// Refresh button (right-aligned). Free-text search lives on the
		// My Projects tab only; the Asset Library uses its tag filter
		// instead, so the only thing in this strip is the refresh action.
		const float refreshW = 90.0f;
		const float availW = ImGui::GetContentRegionAvail().x;
		if (availW > refreshW) {
			ImGui::Dummy(ImVec2(availW - refreshW, 0.0f));
			ImGui::SameLine();
		}
		if (fetchInFlight) ImGui::BeginDisabled();
		if (ImGui::Button(IDX_TR("launcher.asset_library.refresh").c_str(),
			ImVec2(refreshW, strip_h)))
		{
			StartFetchAssetLibraryIndex();
		}
		if (fetchInFlight) ImGui::EndDisabled();
		ImGui::EndGroup();

		// Status line.
		if (fetchInFlight) {
			ImGui::TextDisabled("%s", IDX_TR("launcher.asset_library.progress.fetching_manifest").c_str());
		}
		else if (!fetchError.empty()) {
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "%s",
				IDX_TR("launcher.asset_library.fetch_failed").c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("(%s)", fetchError.c_str());
		}

		// Card list. Each "card" is a Selectable spanning the full row with
		// a darker frame around it. v1 is text-only — no thumbnails yet
		// (that's task #8). The Selectable opens the detail modal.
		ImGui::BeginChild("##AssetLibraryList", ImVec2(0, 0), true);

		std::vector<AssetLibraryEntry> snapshot;
		{
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			snapshot = m_AssetLibrary.Entries;
		}

		size_t shown = 0;
		for (const auto& entry : snapshot) {
			++shown;

			ImGui::PushID(entry.Id.c_str());
			ImGui::Separator();
			ImGui::Spacing();
			if (ImGui::Selectable("##card", false, ImGuiSelectableFlags_AllowOverlap,
				ImVec2(0, 70.0f)))
			{
				m_AssetLibraryDetailEntryId = entry.Id;
				m_AssetLibraryDetailScreenshot = 0;
				m_OpenAssetLibraryDetailPopup = true;
			}
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 66.0f);
			ImGui::Indent();
			ImGui::TextUnformatted(entry.Name.c_str());
			ImGui::SameLine();
			if (!entry.Version.empty()) {
				ImGui::TextDisabled("v%s", entry.Version.c_str());
			}
			if (!entry.Author.empty()) {
				ImGui::TextDisabled("by %s", entry.Author.c_str());
			}
			if (!entry.ShortDescription.empty()) {
				ImGui::TextWrapped("%s", entry.ShortDescription.c_str());
			}
			ImGui::Unindent();
			ImGui::Spacing();
			ImGui::PopID();
		}

		if (shown == 0 && indexLoaded && !fetchInFlight) {
			ImGui::Spacing();
			ImGui::TextDisabled("%s",
				entryCount == 0
					? IDX_TR("launcher.asset_library.coming_soon").c_str()
					: IDX_TR("launcher.asset_library.empty").c_str());
		}

		ImGui::EndChild();

		// Bottom status: error only. Success is surfaced via a modal
		// (RenderAssetLibraryDoneModal); in-flight progress via the shared
		// OS-level popup (Win32BuildProgressWindow) from UpdateProgressPopup.
		AssetLibraryStage stage;
		std::string taskError;
		{
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			stage = m_AssetLibraryTask.Stage;
			taskError = m_AssetLibraryTask.Error;
		}
		if (stage == AssetLibraryStage::Error) {
			ImGui::Separator();
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "%s", taskError.c_str());
		}
	}

	void LauncherLayer::RenderAssetLibraryDetailModal() {
		constexpr const char* k_DetailId = "###AssetLibraryDetail";
		if (m_OpenAssetLibraryDetailPopup) {
			ImGui::OpenPopup(k_DetailId);
			m_OpenAssetLibraryDetailPopup = false;
		}

		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Appearing);

		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		const std::string title = IDX_TR("launcher.asset_library.title") + std::string(k_DetailId);
		if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return;
		}

		AssetLibraryEntry entry;
		bool found = false;
		{
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			if (auto* e = FindAssetLibraryEntry(m_AssetLibraryDetailEntryId)) {
				entry = *e;
				found = true;
			}
		}
		if (!found) {
			ImGui::TextDisabled("%s", IDX_TR("launcher.asset_library.empty").c_str());
			if (ImGui::Button(IDX_TR("launcher.asset_library.close").c_str())) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
			return;
		}

		ImGui::TextUnformatted(entry.Name.c_str());
		if (!entry.Author.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("by %s", entry.Author.c_str());
		}
		ImGui::Separator();

		// Metadata column.
		if (!entry.Version.empty()) ImGui::Text("v%s", entry.Version.c_str());
		if (!entry.License.empty()) ImGui::TextDisabled("License: %s", entry.License.c_str());
		if (!entry.EngineMinVersion.empty()) {
			const bool engineOK = Version::IsAtLeast(IDX_VERSION, entry.EngineMinVersion);
			if (!engineOK) {
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
					"Requires Index %s (you have %s)",
					entry.EngineMinVersion.c_str(), IDX_VERSION);
			}
			else {
				ImGui::TextDisabled("Engine: %s+", entry.EngineMinVersion.c_str());
			}
		}
		if (!entry.RequiredPackages.empty()) {
			std::string joined;
			for (size_t i = 0; i < entry.RequiredPackages.size(); ++i) {
				if (i) joined += ", ";
				joined += entry.RequiredPackages[i];
			}
			ImGui::TextDisabled("Required packages: %s", joined.c_str());
		}

		ImGui::Spacing();
		ImGui::Separator();

		if (!entry.Description.empty()) {
			ImGui::TextWrapped("%s", entry.Description.c_str());
		}

		ImGui::Spacing();
		ImGui::Separator();

		// Bottom action row.
		const bool engineOK = entry.EngineMinVersion.empty()
			|| Version::IsAtLeast(IDX_VERSION, entry.EngineMinVersion);

		bool downloadInFlight = false;
		{
			std::scoped_lock lock(m_AssetLibraryTask.Mutex);
			downloadInFlight = m_AssetLibraryTask.Running;
		}

		if (!engineOK || downloadInFlight) ImGui::BeginDisabled();
		if (ImGui::Button(IDX_TR("launcher.asset_library.download").c_str(), ImVec2(160, 0))) {
			StartAssetLibraryDownload(entry);
			ImGui::CloseCurrentPopup();
		}
		if (!engineOK || downloadInFlight) ImGui::EndDisabled();

		ImGui::SameLine();
		if (ImGui::Button(IDX_TR("launcher.asset_library.close").c_str(), ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void LauncherLayer::RenderAssetLibraryDoneModal() {
		constexpr const char* k_DoneId = "###AssetLibraryDone";

		if (m_OpenAssetLibraryDonePopup) {
			ImGui::OpenPopup(k_DoneId);
			m_OpenAssetLibraryDonePopup = false;
		}

		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);

		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		const std::string title = IDX_TR("launcher.asset_library.done.title") + k_DoneId;
		if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return;
		}

		ImGui::TextWrapped("%s", IDX_TR("launcher.asset_library.done.body").c_str());
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// Centre the button so the dialog stays balanced (matches RenderErrorPopup).
		const float buttonW = 100.0f;
		ImGui::SetCursorPosX((ImGui::GetWindowWidth() - buttonW) * 0.5f);
		if (ImGui::Button(IDX_TR("launcher.asset_library.done.ok").c_str(), ImVec2(buttonW, 0))) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void LauncherLayer::RenderAssetLibraryTrustModal() {
		constexpr const char* k_TrustId = "###AssetLibraryTrust";
		if (m_OpenAssetLibraryTrustPopup) {
			ImGui::OpenPopup(k_TrustId);
			m_OpenAssetLibraryTrustPopup = false;
		}

		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);

		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		const std::string title = IDX_TR("launcher.asset_library.trust.title") + std::string(k_TrustId);
		if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return;
		}

		ImGui::TextWrapped("%s", IDX_TR("launcher.asset_library.trust.body").c_str());
		ImGui::Spacing();
		ImGui::Separator();

		if (ImGui::Button(IDX_TR("launcher.asset_library.trust.acknowledge").c_str(),
			ImVec2(160, 0)))
		{
			m_AssetLibraryTrustAck[ResolveAssetLibraryUrl()] = true;
			SaveLauncherSettings();

			if (m_PendingTrustedDownload.has_value()) {
				AssetLibraryEntry entry = std::move(*m_PendingTrustedDownload);
				m_PendingTrustedDownload.reset();
				ImGui::CloseCurrentPopup();
				StartAssetLibraryDownload(entry);
				ImGui::EndPopup();
				return;
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(IDX_TR("launcher.asset_library.trust.cancel").c_str(),
			ImVec2(120, 0)))
		{
			m_PendingTrustedDownload.reset();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

}
