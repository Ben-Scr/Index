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
#include <vector>

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

		// Public so the anonymous-namespace helpers in LauncherLayer.cpp can
		// see the asset-library data shapes. Not part of any exported
		// engine-side API — they're just internal-to-the-launcher data.
		enum class AssetLibraryStage : std::uint8_t {
			Idle = 0,
			FetchingManifest,
			Downloading,
			Verifying,
			Extracting,
			Importing,
			Done,
			Error,
		};

		struct AssetLibraryEntry {
			std::string Id;
			std::string Name;
			std::string Author;
			std::string ShortDescription;
			std::string Description;
			std::vector<std::string> Tags;
			std::string Category;
			std::string Version;
			std::string License;
			std::string ThumbnailUrl;
			std::vector<std::string> ScreenshotUrls;
			std::string ArchiveUrl;
			std::string ArchiveSha256;
			std::uint64_t ArchiveSizeBytes = 0;
			std::string ArchiveRootInZip;
			std::string EngineMinVersion;
			std::string EngineMaxVersion;
			std::vector<std::string> RequiredPackages;
			std::string Homepage;
			std::string SourceUrl;
		};

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
			bool LaunchRuntime = false;
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

		// Async pipeline for downloading a project from the GitHub-backed
		// asset library. Same shape as OpenProjectTaskState — main thread
		// reads under the mutex each frame, worker writes under the same
		// mutex. The worker captures the entry by value so the index can
		// be re-fetched while a download is in flight.
		struct AssetLibraryTaskState {
			std::mutex Mutex;
			std::thread Worker;
			std::string EntryId;          // index.json entry id
			std::string DisplayName;      // surfaced to the overlay
			AssetLibraryStage Stage = AssetLibraryStage::Idle;
			float Progress = 0.0f;        // 0..1; per-stage for Downloading
			std::string Error;
			std::string TargetProjectPath; // populated once Importing succeeds
			bool Running = false;
			bool Finished = false;
			bool Success = false;
		};

		// Stages of the publish pipeline. Same shape as AssetLibraryStage but
		// runs in the opposite direction (pack → hash → emit instead of
		// download → verify → extract).
		enum class PublishStage : std::uint8_t {
			Idle = 0,
			Zipping,
			Hashing,
			WritingEntry,
			Done,
			Error,
		};

		// Async task for "publish a project as an asset-library entry". The
		// main thread reads under the mutex each frame; the worker calls
		// PackageTool zip → Hash::Sha256OfFile → writes entry.json. Failure
		// modes are reported via Error; on success TargetZipPath +
		// TargetEntryPath point at the artifacts the user should upload.
		struct PublishProjectTaskState {
			std::mutex Mutex;
			std::thread Worker;
			PublishStage Stage = PublishStage::Idle;
			float Progress = 0.0f;
			std::string Error;
			std::string TargetZipPath;
			std::string TargetEntryPath;
			std::string EntryJsonForDisplay;  // pretty-printed, paste-ready
			bool Running = false;
			bool Finished = false;
			bool Success = false;
		};

		struct AssetLibraryIndex {
			int SchemaVersion = 0;
			std::string GeneratedAt;
			std::vector<AssetLibraryEntry> Entries;
			bool Loaded = false;           // false if a fetch hasn't succeeded yet
			bool FetchInFlight = false;
			std::string FetchError;        // last fetch error, if any
			std::chrono::steady_clock::time_point LastRefreshedAt{};
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
		void RenderMyProjectsTab();
		void RenderAssetLibraryTab();
		void RenderAssetLibraryDetailModal();
		void RenderAssetLibraryTrustModal();
		void RenderPublishProjectModal();
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
		void ExecuteProject(const LauncherProjectEntry& entry);
		void OpenProjectInExplorer(const LauncherProjectEntry& entry);
		void BrowseForExistingProject();
		void BrowseForDefaultProjectsLocation();
		void RequestProjectDelete(const LauncherProjectEntry& entry);
		bool DeleteProjectFromDisk(const LauncherProjectEntry& entry);
		void StartCreateProjectAsync(const std::string& name, const std::string& location,
			const std::string& directoryName);
		void PollCreateProjectTask();
		void ResetCreateProjectTask(bool clearWorker = true);
		void StartProjectLaunch(const LauncherProjectEntry& entry, bool launchRuntime);
		void OpenProjectWorkerBody(const LauncherProjectEntry& entry, bool launchRuntime);
		void PollOpenProjectTask();

		// Publish flow: spawn the pack/hash/entry-write worker.
		void RequestPublishProject(const LauncherProjectEntry& entry);
		void StartPublishProject();
		void PublishProjectWorkerBody(std::string projectPath, std::string outputDir,
			std::string id, std::string version, std::string name, std::string author,
			std::string shortDesc, std::string description, std::string tags,
			std::string category, std::string license, std::string archiveRepo,
			std::string archiveTag, std::string engineMin, std::string requiredPackages);
		void PollPublishProjectTask();
		// Sanitize a display name into a kebab-case-ish id.
		static std::string SanitizeId(std::string_view name);

		// Asset library: fetch index, queue downloads, poll worker.
		void StartFetchAssetLibraryIndex();
		void StartAssetLibraryDownload(const AssetLibraryEntry& entry);
		void AssetLibraryDownloadWorkerBody(AssetLibraryEntry entry);
		void PollAssetLibraryTask();
		// Returns the cached entry by id, or nullptr if no longer present.
		const AssetLibraryEntry* FindAssetLibraryEntry(std::string_view id) const;
		// Convenience: build the localized stage string for the overlay.
		std::string AssetLibraryStageText(AssetLibraryStage stage,
			std::string_view displayName) const;
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

		// Asset library state. The index is fetched lazily — either on first
		// switch to the tab, or on user-clicked Refresh. The download task
		// state is single-slot: only one library download is allowed in
		// flight at a time. The "detail" string is the entry id of whatever
		// the user has open in the Detail modal (empty = closed).
		AssetLibraryIndex m_AssetLibrary;
		AssetLibraryTaskState m_AssetLibraryTask;
		std::string m_AssetLibrarySearch;
		std::string m_AssetLibrarySelectedTag;     // empty = "All tags"
		std::string m_AssetLibraryDetailEntryId;
		int m_AssetLibraryDetailScreenshot = 0;
		// Per-source-URL acknowledgement flag for the trust modal. The modal
		// shows once per source URL per machine; ack is persisted in
		// launcher_settings.json so subsequent runs skip it.
		std::unordered_map<std::string, bool> m_AssetLibraryTrustAck;
		// One-shot: when set, the trust modal opens with this entry pending.
		// After ack the worker is kicked off and the field cleared.
		std::optional<AssetLibraryEntry> m_PendingTrustedDownload;
		bool m_OpenAssetLibraryDetailPopup = false;
		bool m_OpenAssetLibraryTrustPopup = false;

		// Publish-to-Library modal. Form buffers live here so they survive
		// re-opens. m_PublishSourceProject is the entry the user clicked
		// Publish on; cleared when the modal closes.
		PublishProjectTaskState m_PublishTask;
		std::optional<LauncherProjectEntry> m_PublishSourceProject;
		bool m_OpenPublishPopup = false;
		char m_PublishId[128]{};
		char m_PublishVersion[32]{};
		char m_PublishName[256]{};
		char m_PublishShort[256]{};
		char m_PublishDescription[1024]{};
		char m_PublishTags[256]{};
		char m_PublishAuthor[128]{};
		char m_PublishLicense[64]{};
		char m_PublishArchiveRepo[256]{};
		char m_PublishArchiveTag[128]{};
		int  m_PublishCategoryIndex = 0;  // 0=Template, 1=Sample, 2=Demo
		std::string m_PublishFormError;

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
		// Per-project process tracker. Each project may have at most one
		// editor process AND one runtime process alive concurrently — the
		// two are tracked in separate slots so launching the runtime via
		// "Execute" doesn't make "Open In Editor" think the project is
		// already open (and vice versa). PID 0 means "no live process in
		// that slot".
		struct RunningProjectProcesses {
			DWORD EditorPid = 0;
			DWORD RuntimePid = 0;
		};
		std::unordered_map<std::string, RunningProjectProcesses> m_RunningProjects;
#endif

		// Modal error dialog. m_ErrorMessage holds the current text;
		// m_OpenErrorPopup is the one-shot "open the popup next frame" flag
		// matching the pattern used by the create/delete/info popups.
		std::string m_ErrorMessage;
		bool m_OpenErrorPopup = false;
	};

}
