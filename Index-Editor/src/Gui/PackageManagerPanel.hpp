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

	// Editor's Package Manager UI — two top-level tabs:
	//
	//   "Search Packages"      — discover + install
	//     ├── Index Packages   collapsing header listing engine-shipped packages
	//     └── User Packages    collapsing header with NuGet / GitHub-URL / Local-path subsections
	//
	//   "Installed Packages"   — review + uninstall, with a top-level search filter
	//     ├── Index Packages   currently-discovered engine packages
	//     └── User Packages    project-local packages + NuGet PackageReferences
	class PackageManagerPanel {
	public:
		void Initialize(PackageManager* manager);
		void Shutdown();
		void Render();

		// Cloud-install introspection — surfaced so the editor's unified
		// loading popup (Win32BuildProgressWindow) can show a progress
		// window without the panel depending on it directly.
		bool IsCloudInstallRunning() const;
		// Snapshots stage + progress under the task mutex. Returns false if
		// the task isn't running (out-params left untouched).
		bool TryGetCloudInstallProgress(std::string& outStage, float& outProgress) const;

	private:
		// ── Top-level tabs ──────────────────────────────────────────────────────────
		void RenderSearchPackagesTab();
		void RenderInstalledPackagesTab();

		// ── Search tab sections ────────────────────────────────────────────────────
		// Merges disk-local engine packages (the historical "Index Packages" list)
		// with entries fetched from the cloud-hosted Index registry. Cloud-only
		// entries render with a "Cloud" badge and an Install button that downloads
		// the package zip into <project>/Packages/<Name>/ before chaining into the
		// normal IndexPackageInstaller::InstallToProject flow.
		void RenderIndexRegistrySection();
		// Cloud-registry plumbing: kick a worker that runs `Index-PackageTool
		// registry-fetch` and parses the result into m_RegistryEntries. Early-
		// returns when the fetch is already in flight or the cache is fresh.
		void RefreshRegistryIfDirty();
		void PollRegistryFetchTask();
		// Spawns a worker that runs `Index-PackageTool registry-download` for
		// the given entry. On completion, PollAutomationTask's continuation
		// runs InstallToProject + StartPostInstallAutomation just like the
		// existing on-disk Install button does.
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
		// Renders one row for an Index package with layer badges + an Install or
		// Uninstall button depending on whether the package is in project.Packages.
		// `mode` selects whether non-installed packages render an Install button
		// (Search tab) or are skipped entirely (Installed tab).
		enum class RowMode { ShowAll, InstalledOnly };
		void RenderIndexPackageRow(const IndexPackageManifest& manifest, const char* idHint, RowMode mode);
		void RenderNugetPackageRow(const PackageInfo& pkg, int index);
		void TriggerNuGetSearch();
		bool BrowseForLocalFolder(std::string& outPath);

		// True if `name` is in the active project's Packages allow-list.
		bool IsPackageInstalled(const std::string& name) const;

		// After a successful Index-package install or removal, refresh editor-side
		// package state without touching the engine solution. The project allow-list
		// and Packages/IndexPackages.props are updated by IndexPackageInstaller.
		void StartPostInstallAutomation();

		// Per-frame poll for the async automate worker; pulls completed results.
		void PollAutomationTask();

		struct AutomationTaskState {
			std::mutex Mutex;
			std::string Stage = "Idle";
			float Progress = 0.0f;
			std::atomic<bool> Running{ false };
			bool Finished = false;
			bool Success = false;
			std::string Error;
		};
		std::shared_ptr<AutomationTaskState> m_AutomationTask;
		std::thread m_AutomationWorker;

		// Win32 IFileOpenDialog::Show is a modal call that blocks the calling thread
		// while the dialog is up — running it on the editor's main thread freezes
		// the entire UI. We spawn a worker that owns its own STA, runs the dialog,
		// and writes the picked path back. The main thread polls and finishes the
		// install once the worker reports Finished.
		struct DiskInstallTaskState {
			std::mutex Mutex;
			std::atomic<bool> Running{ false };
			bool Finished = false;
			std::string PickedPath; // empty if the user cancelled
		};
		std::shared_ptr<DiskInstallTaskState> m_DiskInstallTask;
		std::thread m_DiskInstallWorker;
		void PollDiskInstallTask();

		// Index cloud registry — schema mirrors the JSON served at
		// k_IndexRegistryUrl (PackageManagerPanel.cpp). All fields except
		// Name/Version/DownloadUrl/Sha256 are optional in the registry.
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
			// Which layers the package's index-package.lua declares. Lets the
			// editor render layer badges (native / csharp / native_standalone)
			// on the package row BEFORE the package is downloaded.
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

		// Background download + extract of a single cloud package. Shape
		// mirrors AutomationTaskState — when the worker reports Finished,
		// the main thread calls IndexPackageInstaller::InstallToProject on
		// the newly-extracted directory and then StartPostInstallAutomation.
		struct CloudInstallTaskState {
			std::mutex Mutex;
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

		// Modal popup shown when a cloud install fails. Multi-line errors
		// (network stack traces, SHA mismatches, malformed-zip messages)
		// don't fit cleanly in the bottom status strip, and the strip is
		// easy to miss when a user is focused on the package list. The
		// popup forces acknowledgement.
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

		// "Create new package" wizard state. Layer flags map onto
		// scripts/packages/NewPackage.py's --layers tokens; the target radio decides
		// whether the package is created in the engine packages tree or under
		// the active project's Packages/ folder (the latter is force-disabled
		// when no project is open).
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
