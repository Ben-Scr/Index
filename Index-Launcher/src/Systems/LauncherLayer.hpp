#pragma once
#include "Core/Layer.hpp"
#include "Project/LauncherRegistry.hpp"
#include "Project/IndexProject.hpp"
#include "Core/Export.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#ifdef IDX_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Index {

	class LauncherLayer : public Layer {
	public:
		using Layer::Layer;

		void OnAttach(Application& app) override;
		void OnPreRender(Application& app) override;
		void OnDetach(Application& app) override;

	private:
		struct CreateProjectTaskState {
			std::mutex Mutex;
			std::thread Worker;
			std::optional<IndexProject> CreatedProject;
			std::string Error;
			std::string Stage = "Idle";
			float Progress = 0.0f;
			int BuildExitCode = 0;
			bool Running = false;
			bool Finished = false;
			bool Success = false;
			bool InitialBuildSucceeded = true;
		};

		struct ProjectSizeTaskState {
			std::mutex Mutex;
			bool Finished = false;
			bool Failed = false;
			std::uintmax_t Bytes = 0;
			std::string Error;
			// Worker is declared last so it is destroyed first: the jthread destructor
			// requests stop and joins before the rest of the state is torn down. The
			// worker captures the task by weak_ptr to avoid a shared_ptr cycle.
			std::jthread Worker;
		};

		// Async pipeline run when the user clicks "Open" on a project: regen → build →
		// spawn the editor. Stage/progress are read by the overlay each frame so the UI
		// doesn't freeze during the (potentially seconds-long) MSBuild step.
		struct OpenProjectTaskState {
			std::mutex Mutex;
			std::thread Worker;
			LauncherProjectEntry Entry;
			std::string Stage = "Idle";
			float Progress = 0.0f;
			std::string Error;
			bool Running = false;
			bool Finished = false;
			bool Success = false;
#ifdef IDX_PLATFORM_WINDOWS
			DWORD SpawnedProcessId = 0;
#endif
		};

		// Project sort axis. The launcher persists the user's choice in
		// launcher_settings.json so reopening the launcher keeps the same
		// view. "LastOpened descending" matches the prior implicit default
		// where the registry was sorted on load.
		enum class SortMode : uint8_t {
			LastOpened = 0,
			Name = 1,
			CreatedAt = 2,
		};

		// Multiplier applied to ImGui's FontGlobalScale. The font atlas is
		// baked statically at startup (see comment in ImGuiContextLayer.cpp
		// about ImGuiBackendFlags_RendererHasTextures), so we can only stretch
		// the already-baked glyphs — not re-bake at a different point size.
		// "Auto" currently behaves identically to P100; it exists as a
		// distinct entry so future DPI-driven heuristics can plug in without
		// migrating saved settings.
		enum class FontScale : uint8_t {
			Auto = 0,
			P75 = 1,
			P100 = 2,
			P125 = 3,
			P150 = 4,
			P175 = 5,
			P200 = 6,
		};

		enum class ThemeSetting : uint8_t {
			Auto = 0,
			Dark = 1,
			Light = 2,
		};

		enum class DirectoryNameConvention : uint8_t {
			None = 0,
			TitleCase,
			TitleDashCase,
			TitleCamelCase,
			TitleSnakeCase,
			TitleKebabCase,
		};

		void RenderLauncherPanel();
		void RenderProjectList();
		void RenderCreateProjectPopup();
		void RenderDeleteProjectPopups();
		void RenderSettingsPopup();
		void RenderProjectInfoPopup();
		void RenderErrorPopup();
		void RenderRenameProjectPopup();
		void RequestProjectRename(const LauncherProjectEntry& entry);
		void RemoveProjectFromList(const LauncherProjectEntry& entry);
		const LauncherProjectEntry* GetSelectedProject() const;

		// Queue a one-off error to show in the modal dialog. Replaces the
		// previous "set m_*Error string, render as inline red text" pattern.
		// The dialog opens on the next frame (deferred via m_OpenErrorPopup)
		// so it can be called from inside an ImGui::Begin*-scope safely.
		void ShowError(std::string message);
		void OpenProject(const LauncherProjectEntry& entry);
		void OpenProjectInExplorer(const LauncherProjectEntry& entry);
		void BrowseForExistingProject();
		void BrowseForDefaultProjectsLocation();
		void RequestProjectDelete(const LauncherProjectEntry& entry);
		bool DeleteProjectFromDisk(const LauncherProjectEntry& entry);
		void StartCreateProjectAsync(const std::string& name, const std::string& location,
			const std::string& directoryName);
		void PollCreateProjectTask();
		void ResetCreateProjectTask(bool clearWorker = true);
		void OpenProjectWorkerBody(const LauncherProjectEntry& entry);
		void PollOpenProjectTask();
		std::shared_ptr<ProjectSizeTaskState> GetOrStartProjectSizeTask(const LauncherProjectEntry& entry);
		std::string GetProjectSizeDisplayText(const LauncherProjectEntry& entry);
		std::vector<const LauncherProjectEntry*> GetSortedProjectsView() const;
		void RefreshProjectsList();
		void LoadLauncherSettings();
		void SaveLauncherSettings() const;
		static std::string GetSettingsPath();
		static const char* DirectoryNameConventionLabel(DirectoryNameConvention convention);
		static std::string ApplyDirectoryNameConvention(std::string_view name,
			DirectoryNameConvention convention);
		float GetEffectiveFontScale() const;
		bool IsEffectiveThemeDark() const;
		void ApplyLauncherThemeIfNeeded();

		LauncherRegistry m_Registry;
		std::unordered_map<std::string, std::shared_ptr<ProjectSizeTaskState>> m_ProjectSizeTasks;
		// Cached filesystem creation timestamps keyed by project path, so
		// "Created" sort doesn't stat the disk every frame. Populated lazily.
		mutable std::unordered_map<std::string, std::int64_t> m_CreatedAtCache;

		SortMode m_SortMode = SortMode::LastOpened;
		bool m_SortReverse = false;

		FontScale m_FontScale = FontScale::Auto;
		ThemeSetting m_ThemeSetting = ThemeSetting::Auto;
		DirectoryNameConvention m_DirectoryNameConvention = DirectoryNameConvention::TitleCase;
		std::optional<bool> m_LastAppliedDarkTheme;

		// "Auto" language mode: the launcher resolves the active language to
		// Localization::GetSystemLanguage() at startup and whenever the user
		// re-selects the Auto entry in the combo. Persisted independently of
		// Localization's own locale.json so the resolved code can land there
		// without overriding the user's "follow the OS" intent.
		bool m_LanguageAuto = false;

		std::string m_DefaultProjectsLocation;
		bool m_OpenSettingsPopup = false;

		char m_NewProjectName[256]{};
		char m_NewProjectLocation[512]{};
		std::string m_CreateError;

		bool m_IsCreating = false;
		bool m_OpenCreatePopup = false;
		bool m_CloseCreatePopup = false;
		std::string m_DeferredUpdatePath;
		CreateProjectTaskState m_CreateTask;
		std::optional<LauncherProjectEntry> m_PendingCreatedProjectOpen;

		bool m_IsOpening = false;
		std::chrono::steady_clock::time_point m_OpenStartTime;
		std::string m_OpeningProjectName;
		OpenProjectTaskState m_OpenTask;

		std::optional<LauncherProjectEntry> m_PendingDeleteProject;
		bool m_OpenDeleteConfirmPopup = false;
		bool m_OpenDeleteFinalConfirmPopup = false;
		std::string m_DeleteError;

		// "Project Info" right-click dialog state. m_PendingInfoProject holds the
		// project whose details are being shown; m_OpenInfoPopup is the deferred
		// "open the modal next frame" flag (same pattern as m_OpenDeleteConfirmPopup).
		// m_EngineVersionCache memoises the engineVersion field parsed out of each
		// project's index-project.json so the dialog doesn't re-read the file every
		// frame — invalidated alongside m_CreatedAtCache in RefreshProjectsList.
		std::optional<LauncherProjectEntry> m_PendingInfoProject;
		bool m_OpenInfoPopup = false;
		mutable std::unordered_map<std::string, std::string> m_EngineVersionCache;

		// Rename popup state — mirrors the create-project popup pattern.
		// m_PendingRenameProject holds the entry being renamed (so we can
		// commit by path even if the user shuffles the list while the popup
		// is open); m_RenameBuffer is the editable text field.
		std::optional<LauncherProjectEntry> m_PendingRenameProject;
		bool m_OpenRenamePopup = false;
		char m_RenameBuffer[256]{};

		// Project selection — keyed by path so the selection survives sort
		// changes and registry edits. Empty when nothing is selected. The
		// right-column Open/Rename/Delete buttons gate on this; rows
		// highlight when their path matches.
		std::string m_SelectedProjectPath;

#ifdef IDX_PLATFORM_WINDOWS
		std::unordered_map<std::string, DWORD> m_RunningProjects;
#endif

		// Modal error dialog. m_ErrorMessage holds the current text;
		// m_OpenErrorPopup is the one-shot "open the popup next frame" flag
		// matching the pattern used by the create/delete/info popups.
		std::string m_ErrorMessage;
		bool m_OpenErrorPopup = false;
	};

}
