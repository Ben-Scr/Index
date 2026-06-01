#pragma once
#include "Editor/IndexPackageInstaller.hpp"
#include "Packages/PackageManager.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Index {

	class PackageManagerPanel {
	public:
		void Initialize(PackageManager* manager);
		void Shutdown();
		void Render();

		// True when a cloud-install or post-install automation is in flight; fills title/stage/progress
		// for the shared Win32BuildProgressWindow (editor chrome owns that popup, not the panel).
		bool GetActiveLoadingPopup(std::string& outTitle, std::string& outStage, float& outProgress);

	private:
		// ── Top-level tabs ──────────────────────────────────────────────────────────
		void RenderSearchPackagesTab();
		void RenderInstalledPackagesTab();

		// ── Search tab sections ────────────────────────────────────────────────────
		void RenderIndexRegistrySection();
		void RefreshRegistryIfDirty();
		void PollRegistryFetchTask();
		struct RegistryEntry;
		void HandleCloudInstall(const RegistryEntry& entry);
		// Floating add-package windows opened from the "+" button's drop-down menu.
		void RenderGitInstallWindow();
		void RenderNuGetInstallWindow();
		// "Create new package" wizard — wraps `scripts/packages/NewPackage.py` so the editor
		// and CLI go through the exact same scaffolding code path.
		void RenderNewPackageWindow();
		void HandleNewPackageCreate();
		// Disk-install runs inline (folder picker → install → done), no window.
		void HandleDiskInstall();

		// ── Installed tab sections ─────────────────────────────────────────────────
		void RenderInstalledIndexPackagesSection();
		void RenderInstalledUserPackagesSection();
		void RenderInstalledNuGetPackagesSection();

		// ── Shared helpers ─────────────────────────────────────────────────────────
		void RefreshManifestsIfDirty();
		void RenderLayerBadges(const IndexPackageManifest& manifest);
		// ShowAll = Search tab (Install button for missing); InstalledOnly = Installed tab (skip missing).
		enum class RowMode { ShowAll, InstalledOnly };
		void RenderIndexPackageRow(const IndexPackageManifest& manifest, const char* idHint, RowMode mode);
		void RenderNugetPackageRow(const PackageInfo& pkg, int index);
		void TriggerNuGetSearch();
		bool BrowseForLocalFolder(std::string& outPath);

		// True if `name` is in the active project's Packages allow-list.
		bool IsPackageInstalled(const std::string& name) const;

		// If justInstalledPackageName is non-empty, verifies the package actually loaded after rescan;
		// on SEH-trapped load failure it auto-rolls-back via UninstallFromProject.
		void StartPostInstallAutomation(const std::string& justInstalledPackageName = {});

		// Per-frame poll for the async automate worker; pulls completed results.
		void PollAutomationTask();

		std::string m_PendingPostInstallPackageName;

		struct AutomationTaskState {
			std::mutex Mutex;
			std::string Title = "Refreshing Packages...";
			std::string Stage = "Idle";
			float Progress = 0.0f;
			std::atomic<bool> Running{ false };
			bool Finished = false;
			bool Success = false;
			std::string Error;
		};
		std::shared_ptr<AutomationTaskState> m_AutomationTask;
		std::thread m_AutomationWorker;

		// IFileOpenDialog::Show is modal/blocking — runs in a worker STA thread to avoid freezing the editor UI.
		struct DiskInstallTaskState {
			std::mutex Mutex;
			std::atomic<bool> Running{ false };
			bool Finished = false;
			std::string PickedPath; // empty if the user cancelled
		};
		std::shared_ptr<DiskInstallTaskState> m_DiskInstallTask;
		std::thread m_DiskInstallWorker;
		void PollDiskInstallTask();

		// TODO(registry-v2): add explicit version pinning per project.
		struct RegistryEntry {
			std::string Name;
			std::string Version;
			std::string Description;
			std::string DownloadUrl;
			std::string Sha256;
			std::vector<std::string> Dependencies;
			std::string MinEngineVersion;
			std::string Homepage;
			std::string License;
			std::uint64_t SizeBytes = 0;
			bool HasNativeLayer = false;
			bool HasNativeStandaloneLayer = false;
			bool HasCSharpLayer = false;
		};

		std::vector<RegistryEntry> m_RegistryEntries;
		bool m_RegistryDirty = true;       // true on first open / after manual Refresh
		std::string m_RegistryStatusMessage;
		bool m_RegistryStatusIsError = false;

		// Background fetch of the registry index JSON.
		struct RegistryFetchTaskState {
			std::mutex Mutex;
			std::atomic<bool> Running{ false };
			bool Finished = false;
			bool Success = false;
			std::string Error;
			std::string CachedJsonPath; // path the worker wrote to on success
		};
		std::shared_ptr<RegistryFetchTaskState> m_RegistryFetchTask;
		std::thread m_RegistryFetchWorker;

		// Parse the JSON the C# tool wrote into RegistryEntry rows.
		// Sets outError when the document is missing required structure;
		// silently skips individual malformed entries.
		static void ParseRegistry(const std::string& jsonText,
			std::vector<RegistryEntry>& outEntries,
			std::string& outError);

		struct CloudInstallTaskState {
			std::mutex Mutex;
			std::string Title = "Downloading...";
			std::string Stage = "Idle";
			float Progress = 0.0f;
			std::atomic<bool> Running{ false };
			bool Finished = false;
			bool Success = false;
			std::string Error;
			std::string PackageName; // set on success — drives InstallToProject
		};
		std::shared_ptr<CloudInstallTaskState> m_CloudInstallTask;
		std::thread m_CloudInstallWorker;
		void PollCloudInstallTask();

		// git clone runs in a worker; project-state continuation (InstallToProject + StartPostInstallAutomation) MUST run on the main thread.
		struct GitInstallTaskState {
			std::mutex Mutex;
			std::atomic<bool> Running{ false };
			bool Finished = false;
			bool Success = false;
			std::string Message;
			std::string PackageName;
		};
		std::shared_ptr<GitInstallTaskState> m_GitInstallTask;
		std::thread m_GitInstallWorker;
		void PollGitInstallTask();

		// NewPackage.py runs in a worker; cold Python startup can take seconds and froze the editor when synchronous.
		struct NewPackageTaskState {
			std::mutex Mutex;
			std::atomic<bool> Running{ false };
			bool Finished = false;
			bool Success = false;
			std::string Output;      // formatted scaffolder failure text (empty on success)
			std::string PackageName; // captured at spawn for the install continuation
		};
		std::shared_ptr<NewPackageTaskState> m_NewPackageTask;
		std::thread m_NewPackageWorker;
		void PollNewPackageTask();

		bool m_OpenInstallErrorPopup = false;
		std::string m_InstallErrorMessage;
		void RenderInstallErrorPopup();

		PackageManager* m_Manager = nullptr;

		// Tab + filter state
		int m_TabIndex = 0;
		char m_InstalledFilterBuffer[256]{};
		char m_IndexSearchFilterBuffer[256]{};

		// NuGet sub-panel state (kept compatible with the previous flow)
		int m_SelectedSource = 0;
		char m_NuGetSearchBuffer[256]{};
		std::string m_LastNuGetQuery;
		bool m_IsSearching = false;
		std::future<std::vector<PackageInfo>> m_SearchFuture;
		std::vector<PackageInfo> m_SearchResults;

		bool m_IsOperating = false;
		std::future<PackageOperationResult> m_OperationFuture;
		std::string m_OperationTarget;
		std::string m_OperationVersion;
		bool m_OperationWasInstall = false;

		// Floating-window state for the "+" menu options.
		bool m_ShowGitInstallWindow = false;
		bool m_ShowNuGetInstallWindow = false;
		bool m_ShowNewPackageWindow = false;
		char m_GitHubUrlBuffer[512]{};

		char m_NewPackageNameBuffer[128]{};
		char m_NewPackageDescriptionBuffer[256]{};
		bool m_NewPackageLayerNative = true;
		bool m_NewPackageLayerStandalone = false;
		bool m_NewPackageLayerCsharp = false;
		int  m_NewPackageTarget = 0; // 0 = engine packages/, 1 = <project>/Packages/
		bool m_NewPackageIsCreating = false;
		std::string m_NewPackageError;

		// Status strip at the bottom of the panel
		std::string m_StatusMessage;
		bool m_StatusIsError = false;

		// Cached enumerations
		std::vector<IndexPackageManifest> m_AllManifests;
		bool m_ManifestsDirty = true;

		// NuGet installed cache (still useful for the Installed tab)
		std::vector<PackageInfo> m_InstalledNuGetPackages;
		bool m_InstalledNuGetDirty = true;
	};

}
