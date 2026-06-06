#include <pch.hpp>
#include "Gui/PackageManagerPanel.hpp"

#include "Core/Log.hpp"
#include "Core/PackageHost.hpp"
#include "Gui/ImGuiImplWebGPU.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/Path.hpp"
#include "Utils/PackageToolPath.hpp"
#include "Utils/Process.hpp"

#include <fstream>
#include <sstream>

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>

#ifdef IDX_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace Index {

	namespace {

		constexpr const char* k_IndexRegistryUrl =
			"https://raw.githubusercontent.com/Ben-Scr/Index-PackageRegistry/main/index.json";

		std::string ToLower(std::string value) {
			std::transform(value.begin(), value.end(), value.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}

		bool MatchesFilter(const std::string& haystack, const char* filter) {
			if (!filter || filter[0] == '\0') return true;
			return ToLower(haystack).find(ToLower(filter)) != std::string::npos;
		}

		// NuGet IDs are case-insensitive — dotnet may write different casing than the search API.
		bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
			if (a.size() != b.size()) return false;
			for (size_t i = 0; i < a.size(); ++i) {
				const unsigned char ca = static_cast<unsigned char>(a[i]);
				const unsigned char cb = static_cast<unsigned char>(b[i]);
				if (std::tolower(ca) != std::tolower(cb)) return false;
			}
			return true;
		}

		bool IsSha256Hex(std::string_view value) {
			return value.size() == 64
				&& std::all_of(value.begin(), value.end(), [](unsigned char c) {
					return std::isxdigit(c) != 0;
				});
		}

	} // namespace

	void PackageManagerPanel::Initialize(PackageManager* manager) {
		m_Manager = manager;
		m_AutomationTask = std::make_shared<AutomationTaskState>();
		m_DiskInstallTask = std::make_shared<DiskInstallTaskState>();
		m_RegistryFetchTask = std::make_shared<RegistryFetchTaskState>();
		m_CloudInstallTask = std::make_shared<CloudInstallTaskState>();
		m_GitInstallTask = std::make_shared<GitInstallTaskState>();
		m_NewPackageTask = std::make_shared<NewPackageTaskState>();
	}

	void PackageManagerPanel::Shutdown() {
		// Wait for every worker before tearing down the state they touch.
		if (m_AutomationWorker.joinable()) {
			m_AutomationWorker.join();
		}
		if (m_DiskInstallWorker.joinable()) {
			m_DiskInstallWorker.join();
		}
		if (m_RegistryFetchWorker.joinable()) {
			m_RegistryFetchWorker.join();
		}
		if (m_CloudInstallWorker.joinable()) {
			m_CloudInstallWorker.join();
		}
		if (m_GitInstallWorker.joinable()) {
			m_GitInstallWorker.join();
		}
		if (m_NewPackageWorker.joinable()) {
			m_NewPackageWorker.join();
		}
		// Drain futures explicitly before nulling m_Manager: std::async destructors block, which would stall the UI thread.
		if (m_OperationFuture.valid()) {
			m_OperationFuture.wait();
		}
		if (m_SearchFuture.valid()) {
			m_SearchFuture.wait();
		}
		m_Manager = nullptr;
		m_SearchResults.clear();
		m_InstalledNuGetPackages.clear();
		m_AllManifests.clear();
		m_RegistryEntries.clear();
		m_AutomationTask.reset();
		m_DiskInstallTask.reset();
		m_RegistryFetchTask.reset();
		m_CloudInstallTask.reset();
		m_GitInstallTask.reset();
		m_NewPackageTask.reset();
	}

	void PackageManagerPanel::Render() {
		// Poll async NuGet search/operation futures (legacy flow).
		if (m_IsSearching && m_SearchFuture.valid()) {
			if (m_SearchFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
				m_SearchResults = m_SearchFuture.get();
				m_IsSearching = false;
				m_StatusMessage = std::to_string(m_SearchResults.size()) + " NuGet results";
				m_StatusIsError = false;
			}
		}
		if (m_IsOperating && m_OperationFuture.valid()) {
			if (m_OperationFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
				auto result = m_OperationFuture.get();
				m_IsOperating = false;
				m_StatusMessage = result.Message;
				m_StatusIsError = !result.Success;

				// Refresh from disk now — dotnet may partially write the .csproj on failure.
				if (m_Manager) {
					m_InstalledNuGetPackages = m_Manager->GetInstalledPackages();
					m_InstalledNuGetDirty = false;

					for (auto& pkg : m_SearchResults) {
						bool installed = false;
						std::string installedVersion;
						for (const auto& inst : m_InstalledNuGetPackages) {
							if (EqualsIgnoreCase(pkg.Id, inst.Id)) {
								installed = true;
								installedVersion = inst.Version;
								break;
							}
						}
						pkg.IsInstalled = installed;
						pkg.InstalledVersion = installed ? installedVersion : "";
					}
				}

				if (m_Manager && m_Manager->NeedsReload()) {
					if (ScriptEngine::IsInitialized())
						ScriptEngine::ReloadAssemblies();
					m_Manager->ClearReloadFlag();
				}
			}
		}

		PollAutomationTask();
		PollDiskInstallTask();
		PollRegistryFetchTask();
		PollCloudInstallTask();
		PollGitInstallTask();
		PollNewPackageTask();
		RefreshManifestsIfDirty();
		RefreshRegistryIfDirty();

		// Tab bar without BeginChild: ImGui 1.92 collapsed child content to zero height inside docked tabs.
		if (ImGui::BeginTabBar("##PackageManagerTabs")) {
			if (ImGui::BeginTabItem("Search Packages")) {
				m_TabIndex = 0;
				RenderSearchPackagesTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("In Project")) {
				m_TabIndex = 1;
				RenderInstalledPackagesTab();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		// Status strip — pinned to the bottom of the panel.
		if (m_IsOperating) {
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", m_StatusMessage.c_str());
		}
		else if (m_StatusIsError) {
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", m_StatusMessage.c_str());
		}
		else if (!m_StatusMessage.empty()) {
			ImGui::TextDisabled("%s", m_StatusMessage.c_str());
		}

		// Floating install-windows opened from the "+" menu.
		RenderGitInstallWindow();
		RenderNuGetInstallWindow();
		RenderNewPackageWindow();
		RenderInstallErrorPopup();
	}

	bool PackageManagerPanel::GetActiveLoadingPopup(std::string& outTitle,
		std::string& outStage, float& outProgress) {
		if (m_CloudInstallTask && m_CloudInstallTask->Running.load(std::memory_order_acquire)) {
			std::scoped_lock lock(m_CloudInstallTask->Mutex);
			outTitle = m_CloudInstallTask->Title;
			outStage = m_CloudInstallTask->Stage;
			outProgress = m_CloudInstallTask->Progress;
			return true;
		}
		if (m_AutomationTask && m_AutomationTask->Running.load(std::memory_order_acquire)) {
			std::scoped_lock lock(m_AutomationTask->Mutex);
			outTitle = m_AutomationTask->Title;
			outStage = m_AutomationTask->Stage;
			outProgress = m_AutomationTask->Progress;
			return true;
		}
		return false;
	}

	void PackageManagerPanel::RenderInstallErrorPopup() {
		if (m_OpenInstallErrorPopup) {
			ImGui::OpenPopup("Package install failed");
			m_OpenInstallErrorPopup = false;
		}

		// Center on the next display.
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(720.0f, 600.0f));

		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::BeginPopupModal("Package install failed", nullptr,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Installation could not complete.");
			ImGui::Spacing();
			ImGui::TextUnformatted(m_InstallErrorMessage.c_str());
			ImGui::PopTextWrapPos();

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			const float buttonWidth = 110.0f;
			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - 2 * buttonWidth - spacing + ImGui::GetCursorPosX());

			if (ImGui::Button("Copy details", ImVec2(buttonWidth, 0))) {
				ImGui::SetClipboardText(m_InstallErrorMessage.c_str());
			}
			ImGui::SameLine();
			if (ImGui::Button("Close", ImVec2(buttonWidth, 0)) ||
				ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
				ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
				m_InstallErrorMessage.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	void PackageManagerPanel::RefreshManifestsIfDirty() {
		if (!m_ManifestsDirty) return;
		IndexProject* project = ProjectManager::GetCurrentProject();
		const std::string projectRoot = project ? project->RootDirectory : std::string{};
		m_AllManifests = IndexPackageInstaller::EnumerateAll(projectRoot);
		m_ManifestsDirty = false;
	}

	// ── Search Packages tab ────────────────────────────────────────────────────────

	void PackageManagerPanel::RenderSearchPackagesTab() {
		// Filter row: input fills the row width minus the "+" button.
		const float plusWidth = ImGui::GetFrameHeight();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - plusWidth - ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::InputTextWithHint("##IndexFilter", "Filter packages...",
			m_IndexSearchFilterBuffer, sizeof(m_IndexSearchFilterBuffer));
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);

		const bool automating = m_AutomationTask && m_AutomationTask->Running.load(std::memory_order_acquire);
		// "Create new package" works without a project (engine-developer flow targeting
		// `<repo>/packages/`). The other entries still need a project — they install
		// into the project's csproj / allow-list. Gate them individually inside the popup.
		const bool canOpenMenu = !m_IsOperating && !automating;
		const bool canInstallToProject = ProjectManager::GetCurrentProject() && canOpenMenu;
		if (!canOpenMenu) ImGui::BeginDisabled();
		if (ImGui::Button("+", ImVec2(plusWidth, plusWidth))) {
			ImGui::OpenPopup("##AddPackageMenu");
		}
		if (!canOpenMenu) ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(canOpenMenu ? "Add a package" : "Wait for current operation to finish");
		}

		if (ImGui::BeginPopup("##AddPackageMenu")) {
			if (ImGui::MenuItem("Create new package...")) {
				m_NewPackageNameBuffer[0] = '\0';
				m_NewPackageDescriptionBuffer[0] = '\0';
				m_NewPackageLayerNative = true;
				m_NewPackageLayerStandalone = false;
				m_NewPackageLayerCsharp = false;
				m_NewPackageTarget = ProjectManager::GetCurrentProject() ? 1 : 0;
				m_NewPackageError.clear();
				m_ShowNewPackageWindow = true;
			}
			ImGui::Separator();
			if (!canInstallToProject) ImGui::BeginDisabled();
			if (ImGui::MenuItem("Install package from disk")) {
				HandleDiskInstall();
			}
			if (ImGui::MenuItem("Install package from git URL")) {
				m_GitHubUrlBuffer[0] = '\0';
				m_ShowGitInstallWindow = true;
			}
			if (ImGui::MenuItem("Install package from NuGet")) {
				m_ShowNuGetInstallWindow = true;
			}
			if (!canInstallToProject) ImGui::EndDisabled();
			ImGui::EndPopup();
		}

		ImGui::Spacing();

		ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
		ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
		if (ImGui::CollapsingHeader("Index Registry")) {
			RenderIndexRegistrySection();
		}
	}

	void PackageManagerPanel::RenderIndexRegistrySection() {
		ImGui::Indent();

		// Refresh row — manual re-fetch + status line. The fetch worker runs
		// off the main thread (kicked by RefreshRegistryIfDirty), so the
		// button just sets the dirty flag.
		const bool fetching = m_RegistryFetchTask
			&& m_RegistryFetchTask->Running.load(std::memory_order_acquire);
		if (fetching) ImGui::BeginDisabled();
		if (ImGui::Button("Refresh registry")) {
			m_RegistryDirty = true;
		}
		if (fetching) ImGui::EndDisabled();
		ImGui::SameLine();
		if (fetching) {
			ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Fetching...");
		}
		else if (m_RegistryStatusIsError) {
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
				m_RegistryStatusMessage.c_str());
		}
		else if (!m_RegistryStatusMessage.empty()) {
			ImGui::TextDisabled("%s", m_RegistryStatusMessage.c_str());
		}
		ImGui::Spacing();

		// Track which on-disk engine packages we've already rendered so we
		// can skip duplicate cloud rows for packages already installed/present.
		std::vector<std::string> seenNames;
		seenNames.reserve(m_AllManifests.size());

		int shown = 0;
		for (const auto& manifest : m_AllManifests) {
			if (!manifest.IsEngine) continue;
			seenNames.push_back(manifest.Name);
			if (!MatchesFilter(manifest.Name, m_IndexSearchFilterBuffer)) continue;
			RenderIndexPackageRow(manifest, "search-engine", RowMode::ShowAll);
			++shown;
		}

		// Cloud-only entries: anything in m_RegistryEntries not present on disk.
		IndexProject* project = ProjectManager::GetCurrentProject();
		const bool cloudInstalling = m_CloudInstallTask
			&& m_CloudInstallTask->Running.load(std::memory_order_acquire);
		const bool automating = m_AutomationTask
			&& m_AutomationTask->Running.load(std::memory_order_acquire);
		const bool canInstall = (project != nullptr) && !m_IsOperating && !automating && !cloudInstalling;

		for (const auto& manifest : m_AllManifests) {
			if (manifest.IsEngine) continue;
			const bool inRegistry = std::any_of(m_RegistryEntries.begin(), m_RegistryEntries.end(),
				[&](const RegistryEntry& e) { return e.Name == manifest.Name; });
			if (!inRegistry) continue;
			seenNames.push_back(manifest.Name);
			if (!MatchesFilter(manifest.Name, m_IndexSearchFilterBuffer)) continue;
			RenderIndexPackageRow(manifest, "search-registry", RowMode::ShowAll);
			++shown;
		}

		for (const auto& entry : m_RegistryEntries) {
			if (std::find(seenNames.begin(), seenNames.end(), entry.Name) != seenNames.end()) {
				continue; // already rendered above (engine or installed-from-registry)
			}
			if (!MatchesFilter(entry.Name, m_IndexSearchFilterBuffer)) continue;

			ImGui::PushID(("registry:" + entry.Name).c_str());

			ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s", entry.Name.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("v%s", entry.Version.c_str());

			const float buttonWidth = 90.0f;
			ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth);
			if (!canInstall) ImGui::BeginDisabled();
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
			if (ImGui::Button("Install", ImVec2(buttonWidth, 0))) {
				HandleCloudInstall(entry);
			}
			ImGui::PopStyleColor();
			if (!canInstall) ImGui::EndDisabled();

			// Layer badges. We don't have an on-disk manifest yet, so synthesize
			// one from the registry entry — RenderLayerBadges only reads the
			// three Has*Layer bools (and Name for the imgui id), nothing else.
			IndexPackageManifest pseudoManifest;
			pseudoManifest.Name = entry.Name;
			pseudoManifest.HasNativeLayer = entry.HasNativeLayer;
			pseudoManifest.HasNativeStandaloneLayer = entry.HasNativeStandaloneLayer;
			pseudoManifest.HasCSharpLayer = entry.HasCSharpLayer;
			RenderLayerBadges(pseudoManifest);

			if (!entry.Description.empty()) {
				ImGui::TextWrapped("%s", entry.Description.c_str());
			}
			// Dependency status — flag missing deps but don't auto-install.
			if (!entry.Dependencies.empty()) {
				ImGui::TextDisabled("Depends on:");
				ImGui::SameLine();
				bool first = true;
				for (const auto& dep : entry.Dependencies) {
					if (!first) { ImGui::SameLine(); ImGui::TextDisabled(","); ImGui::SameLine(); }
					const bool depInstalled = IsPackageInstalled(dep);
					ImVec4 color = depInstalled ? ImVec4(0.5f, 0.85f, 0.5f, 1.0f)
												: ImVec4(1.0f, 0.65f, 0.35f, 1.0f);
					ImGui::TextColored(color, "%s%s", dep.c_str(),
						depInstalled ? "" : " (missing)");
					first = false;
				}
			}
			ImGui::Separator();
			ImGui::PopID();
			++shown;
		}

		if (shown == 0) {
			if (m_RegistryEntries.empty() && m_AllManifests.empty()) {
				ImGui::TextDisabled("No packages available yet.");
			}
			else {
				ImGui::TextDisabled("No packages match the filter.");
			}
		}

		ImGui::Unindent();
	}

	void PackageManagerPanel::HandleDiskInstall() {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			m_StatusMessage = "Open a project before installing a package.";
			m_StatusIsError = true;
			return;
		}

		// Already in flight — ignore re-clicks.
		if (m_DiskInstallTask && m_DiskInstallTask->Running.load(std::memory_order_acquire)) {
			return;
		}

		// Join prior worker BEFORE clearing state — Running=false doesn't mean the lambda has unwound.
		if (m_DiskInstallWorker.joinable()) {
			m_DiskInstallWorker.join();
		}

		// Worker initializes STA, runs IFileOpenDialog::Show, signals Finished. Main polls in PollDiskInstallTask.
		{
			std::scoped_lock lock(m_DiskInstallTask->Mutex);
			m_DiskInstallTask->Finished = false;
			m_DiskInstallTask->PickedPath.clear();
		}
		m_DiskInstallTask->Running.store(true, std::memory_order_release);

		m_StatusMessage = "Choose a package folder...";
		m_StatusIsError = false;

		auto state = m_DiskInstallTask;
		m_DiskInstallWorker = std::thread([state]() {
			std::string picked;
#ifdef IDX_PLATFORM_WINDOWS
			HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			const bool ownsCom = SUCCEEDED(initResult);

			IFileOpenDialog* dialog = nullptr;
			HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
				IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog));

			if (SUCCEEDED(hr)) {
				DWORD options = 0;
				dialog->GetOptions(&options);
				dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
				dialog->SetTitle(L"Select an Index Package Folder");
				if (SUCCEEDED(dialog->Show(nullptr))) {
					IShellItem* item = nullptr;
					if (SUCCEEDED(dialog->GetResult(&item)) && item) {
						PWSTR widePath = nullptr;
						if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &widePath)) && widePath) {
							const int len = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
							if (len > 1) {
								std::string utf8(static_cast<size_t>(len - 1), '\0');
								WideCharToMultiByte(CP_UTF8, 0, widePath, -1, utf8.data(), len, nullptr, nullptr);
								picked = std::move(utf8);
							}
							CoTaskMemFree(widePath);
						}
						item->Release();
					}
				}
				dialog->Release();
			}

			if (ownsCom) CoUninitialize();
#endif

			{
				std::scoped_lock lock(state->Mutex);
				// Clear Running inside the mutex so any observer seeing Running==false is guaranteed to see Finished==true.
				state->Running.store(false, std::memory_order_release);
				state->PickedPath = std::move(picked);
				state->Finished = true;
			}
		});
	}

	void PackageManagerPanel::PollDiskInstallTask() {
		if (!m_DiskInstallTask) return;

		bool finished = false;
		std::string picked;
		{
			std::scoped_lock lock(m_DiskInstallTask->Mutex);
			finished = m_DiskInstallTask->Finished;
			if (finished) picked = std::move(m_DiskInstallTask->PickedPath);
		}
		if (!finished) return;

		// Clear Finished so we don't re-process. Worker has already cleared Running.
		{
			std::scoped_lock lock(m_DiskInstallTask->Mutex);
			m_DiskInstallTask->Finished = false;
			m_DiskInstallTask->PickedPath.clear();
		}

		if (m_DiskInstallWorker.joinable()) {
			m_DiskInstallWorker.join();
		}

		// User cancelled — clear transient status.
		if (picked.empty()) {
			if (m_StatusMessage == "Choose a package folder...") {
				m_StatusMessage.clear();
			}
			return;
		}

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			m_StatusMessage = "Project closed before install completed.";
			m_StatusIsError = true;
			return;
		}

		auto result = IndexPackageInstaller::InstallFromLocal(picked, project->PackagesDirectory);
		m_StatusMessage = result.Message;
		m_StatusIsError = !result.Success;
		if (!result.Success) return;

		if (!result.PackageName.empty()) {
			IndexPackageInstaller::InstallToProject(*project, result.PackageName);
		}
		m_ManifestsDirty = true;
		StartPostInstallAutomation(result.PackageName);
	}

	// ── New-Package wizard ──────────────────────────────────────────────────────
	// UI front-end over scripts/packages/NewPackage.py. Uses --no-premake so project installs stay local.
	void PackageManagerPanel::RenderNewPackageWindow() {
		if (!m_ShowNewPackageWindow) return;

		ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::Begin("Create new package", &m_ShowNewPackageWindow,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {

			ImGui::TextWrapped("Scaffolds a new Index package — manifest, source stubs, and "
				"(optionally) the project allow-list entry. Backed by scripts/packages/NewPackage.py.");
			ImGui::Spacing();

			ImGui::TextUnformatted("Name");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputTextWithHint("##NewPkgName", "e.g. Index.Foo or MyGame.Loot",
				m_NewPackageNameBuffer, sizeof(m_NewPackageNameBuffer));
			ImGui::TextDisabled("PascalCase segments separated by '.', e.g. Index.Tilemap2D.");

			ImGui::Spacing();
			ImGui::TextUnformatted("Description (optional)");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputText("##NewPkgDesc", m_NewPackageDescriptionBuffer,
				sizeof(m_NewPackageDescriptionBuffer));

			ImGui::Spacing();
			ImGui::TextUnformatted("Layers");
			ImGui::Indent();
			ImGui::Checkbox("Native (engine_core)##NewPkgNative", &m_NewPackageLayerNative);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("C++ that links Index-Engine and can use IDX_*_TAG, ECS, etc.");
			}
			ImGui::Checkbox("Standalone C++ (no engine link)##NewPkgStandalone",
				&m_NewPackageLayerStandalone);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("C++ that does NOT link Index-Engine. Mutually exclusive with Native.");
			}
			ImGui::Checkbox("C# (.NET 9.0)##NewPkgCsharp", &m_NewPackageLayerCsharp);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Managed assembly (Pkg.<Name>.dll). Auto-bridges to the native sibling "
					"if Native or Standalone is also checked.");
			}
			ImGui::Unindent();

			// Mutual exclusion guardrail mirrors the CLI's parse_layers() check.
			if (m_NewPackageLayerNative && m_NewPackageLayerStandalone) {
				ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.20f, 1.0f),
					"Native and Standalone are mutually exclusive.");
			}

			ImGui::Spacing();
			ImGui::TextUnformatted("Target");
			ImGui::Indent();
			IndexProject* project = ProjectManager::GetCurrentProject();
			ImGui::RadioButton("Engine packages (<repo>/packages/)##NewPkgEngine",
				&m_NewPackageTarget, 0);
			if (!project) ImGui::BeginDisabled();
			ImGui::RadioButton(project
				? "This project (<project>/Packages/)##NewPkgProject"
				: "This project (<no project loaded>)##NewPkgProject",
				&m_NewPackageTarget, 1);
			if (!project) ImGui::EndDisabled();
			ImGui::Unindent();

			if (!m_NewPackageError.empty()) {
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", m_NewPackageError.c_str());
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			const bool automating = m_AutomationTask && m_AutomationTask->Running.load(std::memory_order_acquire);
			const bool layerOk =
				(m_NewPackageLayerNative || m_NewPackageLayerStandalone || m_NewPackageLayerCsharp)
				&& !(m_NewPackageLayerNative && m_NewPackageLayerStandalone);
			const bool nameOk = std::strlen(m_NewPackageNameBuffer) > 0;
			const bool canCreate = layerOk && nameOk && !m_NewPackageIsCreating
				&& !m_IsOperating && !automating
				&& !(m_GitInstallTask && m_GitInstallTask->Running.load(std::memory_order_acquire));

			if (!canCreate) ImGui::BeginDisabled();
			if (ImGui::Button("Create", ImVec2(120, 0))) {
				HandleNewPackageCreate();
			}
			if (!canCreate) ImGui::EndDisabled();

			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0))) {
				m_ShowNewPackageWindow = false;
				m_NewPackageError.clear();
			}
		}
		ImGui::End();
	}

	void PackageManagerPanel::HandleNewPackageCreate() {
		m_NewPackageError.clear();

		const std::string engineRoot = IndexProject::GetEngineRootDir();
		if (engineRoot.empty()) {
			m_NewPackageError = "Engine root not resolved; cannot locate scripts/packages/NewPackage.py.";
			return;
		}

		const std::filesystem::path scriptPath =
			std::filesystem::path(engineRoot) / "scripts" / "packages" / "NewPackage.py";
		if (!std::filesystem::exists(scriptPath)) {
			m_NewPackageError = "scripts/packages/NewPackage.py not found at: " + scriptPath.generic_string();
			return;
		}

		// Comma separator — host shells don't re-parse it on either platform.
		std::vector<std::string> layerTokens;
		if (m_NewPackageLayerNative)     layerTokens.emplace_back("native");
		if (m_NewPackageLayerStandalone) layerTokens.emplace_back("standalone");
		if (m_NewPackageLayerCsharp)     layerTokens.emplace_back("csharp");
		std::string layersArg;
		for (size_t i = 0; i < layerTokens.size(); ++i) {
			if (i > 0) layersArg += ',';
			layersArg += layerTokens[i];
		}

		std::vector<std::string> command = {
			"python",
			scriptPath.generic_string(),
			std::string(m_NewPackageNameBuffer),
			"--layers", layersArg,
			"--no-premake",
		};

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (m_NewPackageTarget == 1) {
			if (!project) {
				m_NewPackageError = "Project target selected but no project is loaded.";
				return;
			}
			command.emplace_back("--project");
			command.emplace_back(project->RootDirectory);
		}

		if (std::strlen(m_NewPackageDescriptionBuffer) > 0) {
			command.emplace_back("--description");
			command.emplace_back(m_NewPackageDescriptionBuffer);
		}

		if (!m_NewPackageTask || m_NewPackageTask->Running.load(std::memory_order_acquire)) {
			return; // a creation is already in flight
		}

		// Capture the package name before the worker runs — the continuation in
		// PollNewPackageTask() clears the wizard buffer on success.
		const std::string newPackageName = m_NewPackageNameBuffer;
		{
			std::scoped_lock lock(m_NewPackageTask->Mutex);
			m_NewPackageTask->Finished = false;
			m_NewPackageTask->Success = false;
			m_NewPackageTask->Output.clear();
			m_NewPackageTask->PackageName = newPackageName;
		}
		m_NewPackageTask->Running.store(true, std::memory_order_release);
		m_NewPackageIsCreating = true;
		m_StatusMessage = std::string("Creating package '") + newPackageName + "'...";
		m_StatusIsError = false;

		if (m_NewPackageWorker.joinable()) m_NewPackageWorker.join();
		auto state = m_NewPackageTask;
		const auto engineRootCopy = engineRoot;
		m_NewPackageWorker = std::thread([state, command, engineRootCopy]() {
			Process::Result result = Process::Run(command, std::filesystem::path(engineRootCopy));
			{
				std::scoped_lock lock(state->Mutex);
				state->Success = result.Succeeded();
				if (result.Succeeded()) {
					state->Output.clear();
				} else {
					state->Output = "Scaffolder failed (exit " + std::to_string(result.ExitCode) +
						"). Output:\n" + result.Output;
				}
				state->Finished = true;
			}
			state->Running.store(false, std::memory_order_release);
		});
	}

	void PackageManagerPanel::RenderGitInstallWindow() {
		if (!m_ShowGitInstallWindow) return;

		ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::Begin("Install package from git URL", &m_ShowGitInstallWindow,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {

			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputTextWithHint("##GitUrl", "https://github.com/owner/repo.git",
				m_GitHubUrlBuffer, sizeof(m_GitHubUrlBuffer));
			ImGui::TextDisabled("The repository's root must contain index-package.lua.");
			ImGui::Spacing();

			IndexProject* project = ProjectManager::GetCurrentProject();
			const bool automating = m_AutomationTask && m_AutomationTask->Running.load(std::memory_order_acquire);
			const bool cloning = m_GitInstallTask && m_GitInstallTask->Running.load(std::memory_order_acquire);
			const bool canInstall = project && std::strlen(m_GitHubUrlBuffer) > 0 && !m_IsOperating && !automating && !cloning;

			if (!canInstall) ImGui::BeginDisabled();
			if (ImGui::Button("Install", ImVec2(120, 0))) {
				const std::string url = m_GitHubUrlBuffer;
				const std::string packagesDir = project->PackagesDirectory;
				if (m_GitInstallTask && !m_GitInstallTask->Running.load(std::memory_order_acquire)) {
					{
						std::scoped_lock lock(m_GitInstallTask->Mutex);
						m_GitInstallTask->Finished = false;
						m_GitInstallTask->Success = false;
						m_GitInstallTask->Message.clear();
						m_GitInstallTask->PackageName.clear();
					}
					m_GitInstallTask->Running.store(true, std::memory_order_release);
					m_StatusMessage = "Cloning " + url + "...";
					m_StatusIsError = false;
					if (m_GitInstallWorker.joinable()) m_GitInstallWorker.join();
					auto state = m_GitInstallTask;
					m_GitInstallWorker = std::thread([state, url, packagesDir]() {
						auto result = IndexPackageInstaller::InstallFromGitHub(url, packagesDir);
						{
							std::scoped_lock lock(state->Mutex);
							state->Success = result.Success;
							state->Message = result.Message;
							state->PackageName = result.PackageName;
							state->Finished = true;
						}
						state->Running.store(false, std::memory_order_release);
					});
				}
			}
			if (!canInstall) ImGui::EndDisabled();

			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0))) {
				m_ShowGitInstallWindow = false;
			}
		}
		ImGui::End();
	}

	void PackageManagerPanel::RenderNuGetInstallWindow() {
		if (!m_ShowNuGetInstallWindow) return;

		ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::Begin("Install package from NuGet", &m_ShowNuGetInstallWindow,
			ImGuiWindowFlags_NoDocking)) {

			if (!m_Manager || !m_Manager->IsReady()) {
				ImGui::TextDisabled("NuGet source unavailable.");
				ImGui::End();
				return;
			}

			m_SelectedSource = 0;

			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
			bool enterPressed = ImGui::InputText("##NuGetSearch", m_NuGetSearchBuffer, sizeof(m_NuGetSearchBuffer),
				ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();

			const bool automating = m_AutomationTask && m_AutomationTask->Running.load(std::memory_order_acquire);
			bool canSearch = !m_IsSearching && !m_IsOperating && !automating && std::strlen(m_NuGetSearchBuffer) > 0;
			if (!canSearch) ImGui::BeginDisabled();
			if (ImGui::Button("Search", ImVec2(60, 0)) || (enterPressed && canSearch)) {
				TriggerNuGetSearch();
			}
			if (!canSearch) ImGui::EndDisabled();

			ImGui::Separator();
			ImGui::BeginChild("##NuGetResults", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
			if (m_IsSearching) {
				ImGui::TextDisabled("Searching...");
			}
			else if (m_SearchResults.empty() && !m_LastNuGetQuery.empty()) {
				ImGui::TextDisabled("No results found.");
			}
			else {
				for (int i = 0; i < static_cast<int>(m_SearchResults.size()); i++) {
					RenderNugetPackageRow(m_SearchResults[i], i);
				}
			}
			ImGui::EndChild();

			if (ImGui::Button("Close", ImVec2(120, 0))) {
				m_ShowNuGetInstallWindow = false;
			}
		}
		ImGui::End();
	}

	// ── Installed Packages tab ─────────────────────────────────────────────────────

	void PackageManagerPanel::RenderInstalledPackagesTab() {
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##InstalledFilter", "Filter installed packages...",
			m_InstalledFilterBuffer, sizeof(m_InstalledFilterBuffer));

		ImGui::Spacing();

		ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
		if (ImGui::CollapsingHeader("Index Packages##installed")) {
			RenderInstalledIndexPackagesSection();
		}

		ImGui::Spacing();

		ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
		if (ImGui::CollapsingHeader("User Packages##installed")) {
			RenderInstalledUserPackagesSection();
		}

		ImGui::Spacing();

		ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
		if (ImGui::CollapsingHeader("NuGet Packages##installed")) {
			RenderInstalledNuGetPackagesSection();
		}
	}

	void PackageManagerPanel::RenderInstalledIndexPackagesSection() {
		ImGui::Indent();

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			ImGui::TextDisabled("Open a project to see its installed Index packages.");
			ImGui::Unindent();
			return;
		}

		auto isFromRegistry = [&](const std::string& name) {
			return std::any_of(m_RegistryEntries.begin(), m_RegistryEntries.end(),
				[&](const RegistryEntry& e) { return e.Name == name; });
		};

		int shown = 0;
		for (const auto& manifest : m_AllManifests) {
			const bool indexPackage = manifest.IsEngine || isFromRegistry(manifest.Name);
			if (!indexPackage) continue;
			if (!IsPackageInstalled(manifest.Name)) continue;
			if (!MatchesFilter(manifest.Name, m_InstalledFilterBuffer)) continue;
			RenderIndexPackageRow(manifest, "installed-index", RowMode::InstalledOnly);
			++shown;
		}
		if (shown == 0) {
			ImGui::TextDisabled("No Index packages installed. Use the Search tab to install one.");
		}

		ImGui::Unindent();
	}

	void PackageManagerPanel::RenderInstalledUserPackagesSection() {
		ImGui::Indent();

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			ImGui::TextDisabled("Open a project to see its installed user packages.");
			ImGui::Unindent();
			return;
		}

		auto isFromRegistry = [&](const std::string& name) {
			return std::any_of(m_RegistryEntries.begin(), m_RegistryEntries.end(),
				[&](const RegistryEntry& e) { return e.Name == name; });
		};
		int shown = 0;
		for (const auto& manifest : m_AllManifests) {
			if (manifest.IsEngine) continue;
			if (isFromRegistry(manifest.Name)) continue;
			if (!IsPackageInstalled(manifest.Name)) continue;
			if (!MatchesFilter(manifest.Name, m_InstalledFilterBuffer)) continue;
			RenderIndexPackageRow(manifest, "installed-user", RowMode::InstalledOnly);
			++shown;
		}
		if (shown == 0) {
			ImGui::TextDisabled("No user packages installed. Add a package via the Search tab.");
		}

		ImGui::Unindent();
	}

	void PackageManagerPanel::RenderInstalledNuGetPackagesSection() {
		ImGui::Indent();

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			ImGui::TextDisabled("Open a project to see its installed NuGet packages.");
			ImGui::Unindent();
			return;
		}

		if (!m_Manager || !m_Manager->IsReady()) {
			ImGui::TextDisabled("NuGet source unavailable.");
			ImGui::Unindent();
			return;
		}

		if (m_InstalledNuGetDirty) {
			m_InstalledNuGetPackages = m_Manager->GetInstalledPackages();
			m_InstalledNuGetDirty = false;
		}

		int shown = 0;
		for (int i = 0; i < static_cast<int>(m_InstalledNuGetPackages.size()); i++) {
			const auto& pkg = m_InstalledNuGetPackages[i];
			if (!MatchesFilter(pkg.Id, m_InstalledFilterBuffer)) continue;
			RenderNugetPackageRow(pkg, i + 100000);
			++shown;
		}
		if (shown == 0) {
			ImGui::TextDisabled("No NuGet packages installed. Use the Search tab's '+' menu to install one.");
		}

		ImGui::Unindent();
	}

	// ── Shared helpers ─────────────────────────────────────────────────────────────

	bool PackageManagerPanel::IsPackageInstalled(const std::string& name) const {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) return false;
		return std::find(project->Packages.begin(), project->Packages.end(), name) != project->Packages.end();
	}

	void PackageManagerPanel::RenderLayerBadges(const IndexPackageManifest& manifest) {
		// engine_core = green, standalone_cpp = blue, csharp = purple.
		auto chip = [](const char* label, ImVec4 color) {
			ImGui::PushStyleColor(ImGuiCol_Button, color);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 1));
			ImGui::SmallButton(label);
			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
		};

		bool any = false;
		if (manifest.HasNativeLayer) {
			if (any) ImGui::SameLine();
			chip("native", ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
			any = true;
		}
		if (manifest.HasNativeStandaloneLayer) {
			if (any) ImGui::SameLine();
			chip("native_standalone", ImVec4(0.20f, 0.40f, 0.65f, 1.0f));
			any = true;
		}
		if (manifest.HasCSharpLayer) {
			if (any) ImGui::SameLine();
			chip("csharp", ImVec4(0.45f, 0.30f, 0.65f, 1.0f));
			any = true;
		}
		if (!any) {
			ImGui::TextDisabled("(no layers declared)");
		}
	}

	void PackageManagerPanel::RenderIndexPackageRow(const IndexPackageManifest& manifest, const char* idHint, RowMode mode) {
		const bool installed = IsPackageInstalled(manifest.Name);
		if (mode == RowMode::InstalledOnly && !installed) {
			return;
		}

		ImGui::PushID((std::string(idHint) + ":" + manifest.Name).c_str());

		ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s", manifest.Name.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("v%s", manifest.Version.c_str());

		IndexProject* project = ProjectManager::GetCurrentProject();
		const float buttonWidth = 90.0f;
		const bool automating = m_AutomationTask && m_AutomationTask->Running.load(std::memory_order_acquire);
		const bool canMutate = (project != nullptr) && !m_IsOperating && !automating;

		if (installed) {
			ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth);
			if (!canMutate) ImGui::BeginDisabled();
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
			if (ImGui::Button("Uninstall", ImVec2(buttonWidth, 0))) {
				auto result = IndexPackageInstaller::UninstallFromProject(*project, manifest.Name);
				m_StatusMessage = result.Message;
				m_StatusIsError = !result.Success;
				if (result.Success) {
					m_ManifestsDirty = true;
					// Uninstall — no package being added, so no rollback check.
					StartPostInstallAutomation();
				}
			}
			ImGui::PopStyleColor();
			if (!canMutate) ImGui::EndDisabled();
		}
		else if (project) {
			ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth);
			if (!canMutate) ImGui::BeginDisabled();
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
			if (ImGui::Button("Install", ImVec2(buttonWidth, 0))) {
				auto result = IndexPackageInstaller::InstallToProject(*project, manifest.Name);
				m_StatusMessage = result.Message;
				m_StatusIsError = !result.Success;
				if (result.Success) {
					m_ManifestsDirty = true;
					StartPostInstallAutomation(manifest.Name);
				}
			}
			ImGui::PopStyleColor();
			if (!canMutate) ImGui::EndDisabled();
		}

		RenderLayerBadges(manifest);
		if (!manifest.Description.empty()) {
			ImGui::TextWrapped("%s", manifest.Description.c_str());
		}
		ImGui::Separator();
		ImGui::PopID();
	}

	void PackageManagerPanel::RenderNugetPackageRow(const PackageInfo& pkg, int index) {
		ImGui::PushID(index);

		if (pkg.Verified) {
			ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "%s", pkg.Id.c_str());
		}
		else {
			ImGui::TextUnformatted(pkg.Id.c_str());
		}
		ImGui::SameLine();
		ImGui::TextDisabled("v%s", pkg.Version.c_str());

		const float buttonWidth = 80.0f;
		ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth);

		bool disabled = m_IsOperating;
		if (disabled) ImGui::BeginDisabled();

		if (pkg.IsInstalled) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
			if (ImGui::Button("Remove", ImVec2(buttonWidth, 0))) {
				m_IsOperating = true;
				m_OperationWasInstall = false;
				m_OperationTarget = pkg.Id;
				m_OperationVersion.clear();
				m_StatusMessage = "Removing " + pkg.Id + "...";
				m_StatusIsError = false;
				m_OperationFuture = m_Manager->RemoveAsync(m_SelectedSource, pkg.Id);
			}
			ImGui::PopStyleColor();
		}
		else {
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
			if (ImGui::Button("Install", ImVec2(buttonWidth, 0))) {
				m_IsOperating = true;
				m_OperationWasInstall = true;
				m_OperationTarget = pkg.Id;
				m_OperationVersion = pkg.Version;
				m_StatusMessage = "Installing " + pkg.Id + " " + pkg.Version + "...";
				m_StatusIsError = false;
				m_OperationFuture = m_Manager->InstallAsync(m_SelectedSource, pkg.Id, pkg.Version);
			}
			ImGui::PopStyleColor();
		}
		if (disabled) ImGui::EndDisabled();

		if (!pkg.Description.empty()) {
			ImGui::TextWrapped("%s", pkg.Description.c_str());
		}
		if (!pkg.Authors.empty() || pkg.TotalDownloads > 0) {
			ImGui::TextDisabled("%s", pkg.Authors.c_str());
			if (pkg.TotalDownloads > 0) {
				ImGui::SameLine();
				if (pkg.TotalDownloads >= 1000000)
					ImGui::TextDisabled("| %.1fM downloads", pkg.TotalDownloads / 1000000.0);
				else if (pkg.TotalDownloads >= 1000)
					ImGui::TextDisabled("| %.1fK downloads", pkg.TotalDownloads / 1000.0);
				else
					ImGui::TextDisabled("| %lld downloads", pkg.TotalDownloads);
			}
		}
		ImGui::Separator();
		ImGui::PopID();
	}

	void PackageManagerPanel::TriggerNuGetSearch() {
		m_LastNuGetQuery = m_NuGetSearchBuffer;
		m_IsSearching = true;
		m_StatusMessage = "Searching NuGet...";
		m_StatusIsError = false;
		m_SearchFuture = m_Manager->SearchAsync(m_SelectedSource, m_LastNuGetQuery, 20);
	}

	bool PackageManagerPanel::BrowseForLocalFolder(std::string& outPath) {
#ifdef IDX_PLATFORM_WINDOWS
		HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		const bool ownsCom = SUCCEEDED(initResult);

		IFileOpenDialog* dialog = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
			IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog));

		bool picked = false;
		if (SUCCEEDED(hr)) {
			DWORD options = 0;
			dialog->GetOptions(&options);
			dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
			dialog->SetTitle(L"Select an Index Package Folder");
			if (SUCCEEDED(dialog->Show(nullptr))) {
				IShellItem* item = nullptr;
				if (SUCCEEDED(dialog->GetResult(&item)) && item) {
					PWSTR widePath = nullptr;
					if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &widePath)) && widePath) {
						const int len = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
						if (len > 1) {
							std::string utf8(static_cast<size_t>(len - 1), '\0');
							WideCharToMultiByte(CP_UTF8, 0, widePath, -1, utf8.data(), len, nullptr, nullptr);
							outPath = std::move(utf8);
							picked = true;
						}
						CoTaskMemFree(widePath);
					}
					item->Release();
				}
			}
			dialog->Release();
		}

		if (ownsCom) CoUninitialize();
		return picked;
#else
		(void)outPath;
		return false;
#endif
	}

	void PackageManagerPanel::StartPostInstallAutomation(const std::string& justInstalledPackageName) {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) return;
		if (!m_AutomationTask) return;
		if (m_AutomationTask->Running.load(std::memory_order_acquire)) return;

		// Track the package name so PollAutomationTask can roll back on load failure; empty = no rollback.
		m_PendingPostInstallPackageName = justInstalledPackageName;

		{
			std::scoped_lock lock(m_AutomationTask->Mutex);
			m_AutomationTask->Title = justInstalledPackageName.empty()
				? std::string("Refreshing Packages...")
				: ("Installing " + justInstalledPackageName + "...");
			m_AutomationTask->Stage = "Refreshing package state...";
			m_AutomationTask->Progress = 0.05f;
			m_AutomationTask->Finished = false;
			m_AutomationTask->Success = false;
			m_AutomationTask->Error.clear();
		}
		m_AutomationTask->Running.store(true, std::memory_order_release);

		// Join any prior worker before spawning a new one.
		if (m_AutomationWorker.joinable()) {
			m_AutomationWorker.join();
		}

		// Capture state by value so the worker is independent of the project
		// lifetime (user may close/switch projects mid-flight).

		// shared_ptr keeps task state alive if panel shuts down mid-flight.
		auto state = m_AutomationTask;
		m_AutomationWorker = std::thread([state]() {
			{
				std::scoped_lock lock(state->Mutex);
				state->Stage = "Refreshing package host...";
				state->Progress = 0.5f;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(80)); // ensure spinner is visible for at least one frame.

			{
				std::scoped_lock lock(state->Mutex);
				state->Progress = 1.0f;
				state->Success = true;
				state->Stage = "Done";
				state->Finished = true;
			}
			state->Running.store(false, std::memory_order_release);
		});
	}

	void PackageManagerPanel::PollAutomationTask() {
		if (!m_AutomationTask) return;

		bool finished = false;
		bool success = false;
		std::string error;
		{
			std::scoped_lock lock(m_AutomationTask->Mutex);
			finished = m_AutomationTask->Finished;
			success = m_AutomationTask->Success;
			error = m_AutomationTask->Error;
		}

		if (!finished) return;

		// Race-protected: only the main thread mutates Finished back to false.
		{
			std::scoped_lock lock(m_AutomationTask->Mutex);
			m_AutomationTask->Finished = false;
		}

		if (m_AutomationWorker.joinable()) {
			m_AutomationWorker.join();
		}

		// Clear up-front so a fault in LoadInstalled doesn't leave the name and cause a phantom rollback later.
		const std::string pendingInstall = std::move(m_PendingPostInstallPackageName);
		m_PendingPostInstallPackageName.clear();

		if (success) {
			const size_t newlyLoaded = PackageHost::LoadInstalled();

			// SEH rollback: if the DLL failed to load it stays in project.json causing repeated crash-on-open;
			// remove it from the allow-list immediately so the project stays usable.
			bool didRollback = false;
			if (!pendingInstall.empty() && !PackageHost::IsPackageLoaded(pendingInstall)) {
				IndexProject* project = ProjectManager::GetCurrentProject();
				if (project) {
					IndexPackageInstaller::UninstallFromProject(*project, pendingInstall);
					didRollback = true;
				}
			}

			if (didRollback) {
				m_StatusMessage.clear();
				m_StatusIsError = false;
				m_InstallErrorMessage =
					"The package '" + pendingInstall + "' was downloaded and verified, but failed to load "
					"into the running editor. The most common cause is a binary-compatibility mismatch — "
					"the package's published DLL was built against a different version of Index-Engine.\n\n"
					"The package has been removed from this project's allow-list, so your project is safe "
					"and will open normally next time. To install it, ask the publisher to rebuild against "
					"the current engine, or check the package's release notes for a compatible version.\n\n"
					"See the editor's log for the exact failure mode (see lines tagged [PackageHost]).";
				m_OpenInstallErrorPopup = true;
			}
			else if (newlyLoaded > 0) {
				m_StatusMessage = "Package operation complete; loaded " +
					std::to_string(newlyLoaded) + " new package(s). "
					"Reload the .csproj in your C# IDE if a new namespace doesn't resolve.";
				m_StatusIsError = false;
			}
			else {
				m_StatusMessage = "Package operation complete; project package references refreshed. "
					"Reload the .csproj in your C# IDE if a new namespace doesn't resolve.";
				m_StatusIsError = false;
			}
		}
		else {
			// Premake/MSBuild output can be hundreds of lines — surface the full
			// text in the modal popup. Don't echo a redundant one-liner in the
			// bottom strip; the popup is the canonical channel for errors.
			m_StatusMessage.clear();
			m_StatusIsError = false;
			m_InstallErrorMessage = error.empty() ? std::string("Package automation failed.") : error;
			m_OpenInstallErrorPopup = true;
		}

		m_ManifestsDirty = true;
	}

	void PackageManagerPanel::PollGitInstallTask() {
		if (!m_GitInstallTask) return;

		bool finished = false, success = false;
		std::string message, packageName;
		{
			std::scoped_lock lock(m_GitInstallTask->Mutex);
			finished = m_GitInstallTask->Finished;
			success = m_GitInstallTask->Success;
			message = m_GitInstallTask->Message;
			packageName = m_GitInstallTask->PackageName;
		}
		if (!finished) return;
		{
			std::scoped_lock lock(m_GitInstallTask->Mutex);
			m_GitInstallTask->Finished = false;
		}
		if (m_GitInstallWorker.joinable()) m_GitInstallWorker.join();

		m_StatusMessage = message;
		m_StatusIsError = !success;
		if (success) {
			// Continuation on the main thread, mirroring the old inline flow:
			// register the cloned package with the project + kick automation.
			IndexProject* project = ProjectManager::GetCurrentProject();
			if (project && !packageName.empty()) {
				IndexPackageInstaller::InstallToProject(*project, packageName);
			}
			m_ManifestsDirty = true;
			m_GitHubUrlBuffer[0] = '\0';
			m_ShowGitInstallWindow = false;
			StartPostInstallAutomation(packageName);
		}
	}

	void PackageManagerPanel::PollNewPackageTask() {
		if (!m_NewPackageTask) return;

		bool finished = false, success = false;
		std::string output, packageName;
		{
			std::scoped_lock lock(m_NewPackageTask->Mutex);
			finished = m_NewPackageTask->Finished;
			success = m_NewPackageTask->Success;
			output = m_NewPackageTask->Output;
			packageName = m_NewPackageTask->PackageName;
		}
		if (!finished) return;
		{
			std::scoped_lock lock(m_NewPackageTask->Mutex);
			m_NewPackageTask->Finished = false;
		}
		if (m_NewPackageWorker.joinable()) m_NewPackageWorker.join();

		m_NewPackageIsCreating = false;

		if (!success) {
			m_NewPackageError = output.empty() ? std::string("Package creation failed.") : output;
			IDX_ERROR_TAG("IndexPackages", "{}", m_NewPackageError);
			m_StatusMessage = "Package creation failed.";
			m_StatusIsError = true;
			return;
		}

		IDX_INFO_TAG("IndexPackages", "Created package '{}'.", packageName);

		// Project-local package creation updates the project allow-list + props,
		// but the editor must not regenerate the engine solution.
		m_ManifestsDirty = true;
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (project) {
			auto installResult = IndexPackageInstaller::InstallToProject(*project, packageName);
			m_StatusMessage = installResult.Message;
			m_StatusIsError = !installResult.Success;
			if (installResult.Success) {
				StartPostInstallAutomation(packageName);
			}
		}
		else {
			m_StatusMessage = std::string("Package '") + packageName +
				"' created. Run premake5 vs2022 + rebuild the engine solution to pick it up.";
			m_StatusIsError = false;
		}

		m_ShowNewPackageWindow = false;
		m_NewPackageNameBuffer[0] = '\0';
		m_NewPackageDescriptionBuffer[0] = '\0';
	}

	// ── Cloud Registry ─────────────────────────────────────────────────────────────

	namespace {
		bool ReadFileUtf8(const std::filesystem::path& path, std::string& outText) {
			std::ifstream in(path, std::ios::binary);
			if (!in.is_open()) return false;
			std::ostringstream ss;
			ss << in.rdbuf();
			outText = ss.str();
			return true;
		}

		std::filesystem::path RegistryCachePath(const IndexProject& project) {
			return std::filesystem::path(project.RootDirectory) / ".index" / "registry-cache.json";
		}

		std::vector<std::string> BuildPackageToolBase(const std::filesystem::path& toolPath) {
			std::vector<std::string> command;
			if (toolPath.extension() == ".dll") {
				command.push_back("dotnet");
				command.push_back(toolPath.string());
			}
			else {
				command.push_back(toolPath.string());
			}
			return command;
		}
	} // namespace

	void PackageManagerPanel::ParseRegistry(const std::string& jsonText,
		std::vector<RegistryEntry>& outEntries,
		std::string& outError) {
		outEntries.clear();
		std::string parseError;
		Json::Value root;
		if (!Json::TryParse(jsonText, root, &parseError)) {
			outError = "Registry JSON parse failed: " + parseError;
			return;
		}
		if (!root.IsObject()) {
			outError = "Registry JSON root is not an object.";
			return;
		}
		const Json::Value* packagesNode = root.FindMember("packages");
		if (!packagesNode || !packagesNode->IsArray()) {
			outError = "Registry JSON missing 'packages' array.";
			return;
		}
		for (const Json::Value& pkg : packagesNode->GetArray()) {
			if (!pkg.IsObject()) continue;
			RegistryEntry entry;
			if (auto* v = pkg.FindMember("name"))             entry.Name = v->AsStringOr();
			if (auto* v = pkg.FindMember("version"))          entry.Version = v->AsStringOr();
			if (auto* v = pkg.FindMember("description"))      entry.Description = v->AsStringOr();
			if (auto* v = pkg.FindMember("downloadUrl"))      entry.DownloadUrl = v->AsStringOr();
			if (auto* v = pkg.FindMember("sha256"))           entry.Sha256 = v->AsStringOr();
			if (auto* v = pkg.FindMember("homepage"))         entry.Homepage = v->AsStringOr();
			if (auto* v = pkg.FindMember("license"))          entry.License = v->AsStringOr();
			if (auto* v = pkg.FindMember("minEngineVersion")) entry.MinEngineVersion = v->AsStringOr();
			if (auto* v = pkg.FindMember("sizeBytes"))        entry.SizeBytes = v->AsUInt64Or(0);
			if (auto* v = pkg.FindMember("dependencies"); v && v->IsArray()) {
				for (const auto& dep : v->GetArray()) {
					if (dep.IsString()) entry.Dependencies.push_back(dep.AsStringOr());
				}
			}
			// "layers": ["native", "csharp", ...] — fold legacy names too so
			// older registries that still use engine_core / standalone_cpp
			// don't lose their badges.
			if (auto* v = pkg.FindMember("layers"); v && v->IsArray()) {
				for (const auto& layer : v->GetArray()) {
					if (!layer.IsString()) continue;
					const std::string name = layer.AsStringOr();
					if (name == "native" || name == "engine_core") entry.HasNativeLayer = true;
					else if (name == "native_standalone" || name == "standalone_cpp") entry.HasNativeStandaloneLayer = true;
					else if (name == "csharp") entry.HasCSharpLayer = true;
				}
			}
			if (entry.Name.empty() || entry.Version.empty() || entry.DownloadUrl.empty()
				|| !IsSha256Hex(entry.Sha256)) {
				continue; // skip malformed rows; required fields missing
			}
			outEntries.push_back(std::move(entry));
		}
	}

	void PackageManagerPanel::RefreshRegistryIfDirty() {
		if (!m_RegistryDirty) return;
		if (!m_RegistryFetchTask) return;
		if (m_RegistryFetchTask->Running.load(std::memory_order_acquire)) return;

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			m_RegistryDirty = false;
			m_RegistryEntries.clear();
			m_RegistryStatusMessage = "Open a project to browse the Index registry.";
			m_RegistryStatusIsError = false;
			return;
		}

		const std::filesystem::path toolPath = Index::PackageTool::ResolveExecutable();
		if (toolPath.empty()) {
			m_RegistryDirty = false;
			m_RegistryStatusMessage = "Index-PackageTool not found — cannot fetch registry.";
			m_RegistryStatusIsError = true;
			return;
		}

		const std::filesystem::path cachePath = RegistryCachePath(*project);
		std::error_code ec;
		std::filesystem::create_directories(cachePath.parent_path(), ec);

		// Join the prior worker before mutating state.
		if (m_RegistryFetchWorker.joinable()) {
			m_RegistryFetchWorker.join();
		}

		{
			std::scoped_lock lock(m_RegistryFetchTask->Mutex);
			m_RegistryFetchTask->Finished = false;
			m_RegistryFetchTask->Success = false;
			m_RegistryFetchTask->Error.clear();
			m_RegistryFetchTask->CachedJsonPath = cachePath.string();
		}
		m_RegistryFetchTask->Running.store(true, std::memory_order_release);
		m_RegistryDirty = false;
		m_RegistryStatusMessage = "Fetching registry...";
		m_RegistryStatusIsError = false;

		auto state = m_RegistryFetchTask;
		const std::string url = k_IndexRegistryUrl;
		const std::string cachePathStr = cachePath.string();
		m_RegistryFetchWorker = std::thread([state, toolPath, url, cachePathStr]() {
			std::vector<std::string> command = BuildPackageToolBase(toolPath);
			command.push_back("registry-fetch");
			command.push_back(url);
			command.push_back(cachePathStr);

			const Process::Result result = Process::Run(command, {},
				std::chrono::milliseconds(30000));

			std::scoped_lock lock(state->Mutex);
			state->Success = result.Succeeded();
			if (!state->Success) {
				if (result.TimedOut && result.Output.empty()) {
					state->Error = "Registry fetch timed out.";
				}
				else {
					state->Error = result.Output.empty()
						? std::string("Registry fetch failed (exit ")
							+ std::to_string(result.ExitCode) + ")."
						: result.Output;
				}
			}
			state->Finished = true;
			state->Running.store(false, std::memory_order_release);
		});
	}

	void PackageManagerPanel::PollRegistryFetchTask() {
		if (!m_RegistryFetchTask) return;

		bool finished = false;
		bool success = false;
		std::string error;
		std::string cachePath;
		{
			std::scoped_lock lock(m_RegistryFetchTask->Mutex);
			finished = m_RegistryFetchTask->Finished;
			success = m_RegistryFetchTask->Success;
			error = m_RegistryFetchTask->Error;
			cachePath = m_RegistryFetchTask->CachedJsonPath;
		}
		if (!finished) return;

		{
			std::scoped_lock lock(m_RegistryFetchTask->Mutex);
			m_RegistryFetchTask->Finished = false;
		}

		if (m_RegistryFetchWorker.joinable()) {
			m_RegistryFetchWorker.join();
		}

		// On either outcome, try to load from cache: a fresh fetch wrote
		// the file; a failed fetch falls back to whatever was cached on a
		// prior session (offline-first behavior).
		std::string jsonText;
		const bool haveCache = ReadFileUtf8(cachePath, jsonText);

		if (success && haveCache) {
			std::string parseError;
			ParseRegistry(jsonText, m_RegistryEntries, parseError);
			if (!parseError.empty()) {
				m_RegistryStatusMessage = parseError;
				m_RegistryStatusIsError = true;
			}
			else {
				m_RegistryStatusMessage = std::to_string(m_RegistryEntries.size())
					+ " package(s) in registry.";
				m_RegistryStatusIsError = false;
			}
		}
		else if (!success && haveCache) {
			std::string parseError;
			ParseRegistry(jsonText, m_RegistryEntries, parseError);
			m_RegistryStatusMessage = parseError.empty()
				? std::string("Using cached registry (offline).")
				: parseError;
			m_RegistryStatusIsError = !parseError.empty();
		}
		else {
			m_RegistryEntries.clear();
			m_RegistryStatusMessage = error.empty()
				? std::string("Could not reach registry; no cache available.")
				: error;
			m_RegistryStatusIsError = true;
		}
	}

	void PackageManagerPanel::HandleCloudInstall(const RegistryEntry& entry) {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			m_StatusMessage = "Open a project before installing a package.";
			m_StatusIsError = true;
			return;
		}
		if (!m_CloudInstallTask) return;
		if (m_CloudInstallTask->Running.load(std::memory_order_acquire)) return;
		if (!IsSha256Hex(entry.Sha256)) {
			m_StatusMessage = "Registry package '" + entry.Name + "' has no valid SHA-256 digest.";
			m_StatusIsError = true;
			return;
		}

		const std::filesystem::path toolPath = Index::PackageTool::ResolveExecutable();
		if (toolPath.empty()) {
			m_StatusMessage = "Index-PackageTool not found.";
			m_StatusIsError = true;
			return;
		}

		// Join the prior worker before mutating state.
		if (m_CloudInstallWorker.joinable()) {
			m_CloudInstallWorker.join();
		}

		{
			std::scoped_lock lock(m_CloudInstallTask->Mutex);
			m_CloudInstallTask->Title = "Downloading " + entry.Name + "...";
			m_CloudInstallTask->Stage = "Downloading " + entry.Name + " v" + entry.Version + "...";
			m_CloudInstallTask->Progress = 0.1f;
			m_CloudInstallTask->Finished = false;
			m_CloudInstallTask->Success = false;
			m_CloudInstallTask->Error.clear();
			m_CloudInstallTask->PackageName.clear();
		}
		m_CloudInstallTask->Running.store(true, std::memory_order_release);

		m_StatusMessage = "Installing " + entry.Name + " from registry...";
		m_StatusIsError = false;

		auto state = m_CloudInstallTask;
		const std::string packagesDir = project->PackagesDirectory;
		const std::string expectedName = entry.Name;
		const std::string downloadUrl = entry.DownloadUrl;
		const std::string sha256 = entry.Sha256;
		m_CloudInstallWorker = std::thread([state, toolPath, packagesDir, downloadUrl, sha256, expectedName]() {
			std::vector<std::string> command = BuildPackageToolBase(toolPath);
			command.push_back("registry-download");
			command.push_back(downloadUrl);
			command.push_back(sha256);
			command.push_back(packagesDir);

			{
				std::scoped_lock lock(state->Mutex);
				state->Stage = "Downloading " + expectedName + "...";
				state->Progress = 0.3f;
			}

			// Bounded timeout (not 0 = wait-forever): a stalled registry download would
			// otherwise pin m_CloudInstallWorker, and Shutdown()'s unconditional join()
			// would hang the editor on exit. 180s is generous for large artifacts; the
			// sibling registry-fetch uses 30s.
			const Process::Result result = Process::Run(command, {}, std::chrono::milliseconds(180000));

			std::scoped_lock lock(state->Mutex);
			state->Success = result.Succeeded();
			if (state->Success) {
				state->Stage = "Verified + extracted " + expectedName;
				state->Progress = 0.9f;
				state->PackageName = expectedName;
			}
			else {
				if (result.TimedOut && result.Output.empty()) {
					state->Error = "Cloud install timed out.";
				}
				else {
					state->Error = result.Output.empty()
						? std::string("Cloud install failed (exit ")
							+ std::to_string(result.ExitCode) + ")."
						: result.Output;
				}
				state->Progress = 0.0f;
			}
			state->Finished = true;
			state->Running.store(false, std::memory_order_release);
		});
	}

	void PackageManagerPanel::PollCloudInstallTask() {
		if (!m_CloudInstallTask) return;

		bool finished = false;
		bool success = false;
		std::string error;
		std::string packageName;
		{
			std::scoped_lock lock(m_CloudInstallTask->Mutex);
			finished = m_CloudInstallTask->Finished;
			success = m_CloudInstallTask->Success;
			error = m_CloudInstallTask->Error;
			packageName = m_CloudInstallTask->PackageName;
		}
		if (!finished) return;

		{
			std::scoped_lock lock(m_CloudInstallTask->Mutex);
			m_CloudInstallTask->Finished = false;
		}

		if (m_CloudInstallWorker.joinable()) {
			m_CloudInstallWorker.join();
		}

		if (!success) {
			m_StatusMessage.clear();
			m_StatusIsError = false;
			m_InstallErrorMessage = error.empty() ? std::string("Cloud install failed.") : error;
			m_OpenInstallErrorPopup = true;
			return;
		}

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			m_StatusMessage = "Downloaded " + packageName + " but no project is open to install it into.";
			m_StatusIsError = true;
			return;
		}
		const auto result = IndexPackageInstaller::InstallToProject(*project, packageName);
		if (!result.Success) {
			m_StatusMessage = "Downloaded " + packageName + " but allow-list update failed: " + result.Message;
			m_StatusIsError = true;
			return;
		}

		m_ManifestsDirty = true;
		m_StatusMessage = "Installed " + packageName + " from registry.";
		m_StatusIsError = false;
		// Pass the package name so the SEH-guarded LoadInstalled() can roll
		// back the project's allow-list if the just-downloaded DLL fails to
		// load (the Tilemap2D ABI-mismatch scenario this safety net targets).
		StartPostInstallAutomation(packageName);
	}

}
