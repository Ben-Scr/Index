#include <pch.hpp>
#include "Systems/ImGuiEditorLayer.hpp"

#include <imgui.h>

#include "Assets/AssetRegistry.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/EditorIcons.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Inspector/ReferencePicker.hpp"
#include "Packages/GitHubSource.hpp"
#include "Packages/NuGetSource.hpp"
#include "Project/AxiomProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptDiscovery.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scripting/ScriptSystem.hpp"
#include "Serialization/Directory.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Utils/Process.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_set>

namespace Axiom {
	namespace {
		bool NeedsCopy(const std::filesystem::path& src, const std::filesystem::path& dest) {
			if (!std::filesystem::exists(dest)) return true;
			try {
				return std::filesystem::last_write_time(src) > std::filesystem::last_write_time(dest);
			}
			catch (...) {
				return true;
			}
		}

		int CopyDirIncremental(const std::filesystem::path& srcDir, const std::filesystem::path& destDir) {
			int copied = 0;
			std::filesystem::create_directories(destDir);
			for (auto& entry : std::filesystem::recursive_directory_iterator(srcDir)) {
				auto rel = std::filesystem::relative(entry.path(), srcDir);
				auto dest = destDir / rel;
				try {
					if (entry.is_directory()) {
						std::filesystem::create_directories(dest);
					}
					else if (NeedsCopy(entry.path(), dest)) {
						std::filesystem::create_directories(dest.parent_path());
						std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
						copied++;
					}
				}
				catch (...) {
				}
			}
			return copied;
		}

		// Variant that skips any file or directory whose path-relative-to-srcDir
		// has a leading segment matching one of `excludedSegments`. Used to keep
		// editor-only assets (AxiomAssets/Textures/Editor) out of shipped builds.
		// Match is case-insensitive on Windows so the Editor folder excludes
		// regardless of how the user cased it on disk.
		int CopyDirIncrementalExcluding(const std::filesystem::path& srcDir,
			const std::filesystem::path& destDir,
			const std::vector<std::filesystem::path>& excludedRelativePaths)
		{
			int copied = 0;
			std::filesystem::create_directories(destDir);

			auto isExcluded = [&](const std::filesystem::path& rel) {
				const std::string relStr = rel.generic_string();
				for (const auto& excluded : excludedRelativePaths) {
					const std::string excStr = excluded.generic_string();
					// Match the excluded path itself or anything underneath it.
#ifdef AIM_PLATFORM_WINDOWS
					if (relStr.size() >= excStr.size()
						&& _strnicmp(relStr.c_str(), excStr.c_str(), excStr.size()) == 0
						&& (relStr.size() == excStr.size() || relStr[excStr.size()] == '/'))
					{
						return true;
					}
#else
					if (relStr.size() >= excStr.size()
						&& relStr.compare(0, excStr.size(), excStr) == 0
						&& (relStr.size() == excStr.size() || relStr[excStr.size()] == '/'))
					{
						return true;
					}
#endif
				}
				return false;
			};

			for (auto it = std::filesystem::recursive_directory_iterator(srcDir);
				it != std::filesystem::recursive_directory_iterator(); ++it)
			{
				auto rel = std::filesystem::relative(it->path(), srcDir);
				if (isExcluded(rel)) {
					if (it->is_directory()) {
						it.disable_recursion_pending();
					}
					continue;
				}
				auto dest = destDir / rel;
				try {
					if (it->is_directory()) {
						std::filesystem::create_directories(dest);
					}
					else if (NeedsCopy(it->path(), dest)) {
						std::filesystem::create_directories(dest.parent_path());
						std::filesystem::copy_file(it->path(), dest, std::filesystem::copy_options::overwrite_existing);
						copied++;
					}
				}
				catch (...) {
				}
			}
			return copied;
		}

		std::string GetRuntimeExecutableFilename() {
#if defined(AIM_PLATFORM_WINDOWS)
			return "Axiom-Runtime.exe";
#else
			return "Axiom-Runtime";
#endif
		}

		std::string GetEngineRuntimeFilename() {
#if defined(AIM_PLATFORM_WINDOWS)
			return "Axiom-Engine.dll";
#elif defined(__APPLE__)
			return "libAxiom-Engine.dylib";
#else
			return "libAxiom-Engine.so";
#endif
		}

		std::string GetNetHostRuntimeFilename() {
#if defined(AIM_PLATFORM_WINDOWS)
			return "nethost.dll";
#else
			return "libnethost.so";
#endif
		}

		// Filename of a premake `kind "SharedLib"` target on the current platform.
		// Windows: <Name>.dll, Linux: lib<Name>.so, macOS: lib<Name>.dylib.
		std::string SharedLibraryFilename(std::string_view projectName) {
#if defined(AIM_PLATFORM_WINDOWS)
			return std::string(projectName) + ".dll";
#elif defined(__APPLE__)
			return "lib" + std::string(projectName) + ".dylib";
#else
			return "lib" + std::string(projectName) + ".so";
#endif
		}

		std::string NormalizePreviewTexturePath(const std::filesystem::path& path) {
			std::error_code ec;
			std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
			if (ec) {
				ec.clear();
				normalized = std::filesystem::absolute(path, ec);
				if (ec) {
					normalized = path.lexically_normal();
				}
			}

			return normalized.lexically_normal().make_preferred().string();
		}

		bool IsLogEntryVisible(Log::Level level, bool showInfo, bool showWarn, bool showError) {
			if (level <= Log::Level::Info) return showInfo;
			if (level == Log::Level::Warn) return showWarn;
			return showError;
		}

		const char* GetLogLevelPrefix(Log::Level level) {
			if (level <= Log::Level::Info) return "[Info] ";
			if (level == Log::Level::Warn) return "[Warn] ";
			return "[Error] ";
		}

		ImVec4 GetLogLevelColor(Log::Level level) {
			if (level <= Log::Level::Info) return ImVec4(0.86f, 0.88f, 0.92f, 1.0f);
			if (level == Log::Level::Warn) return ImVec4(1.0f, 0.78f, 0.22f, 1.0f);
			return ImVec4(1.0f, 0.32f, 0.32f, 1.0f);
		}
	}

	void ImGuiEditorLayer::RenderLogPanel() {
		DrainPendingLogEntries();
		ImGui::Begin("Log");

		if (ImGui::Button("Clear")) {
			ClearLogEntries();
		}

		int infoCount = 0, warnCount = 0, errorCount = 0;
		for (const auto& entry : m_LogEntries) {
			if (entry.Level <= Log::Level::Info) infoCount++;
			else if (entry.Level == Log::Level::Warn) warnCount++;
			else errorCount++;
		}

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		{
			unsigned int infoIcon = EditorIcons::Get("info", 16);
			if (infoIcon) {
				ImVec4 tint = m_ShowLogInfo ? ImVec4(1, 1, 1, 1) : ImVec4(0.4f, 0.4f, 0.4f, 0.5f);
				if (ImGui::ImageButton("##FilterInfo",
					static_cast<ImTextureID>(static_cast<intptr_t>(infoIcon)),
					ImVec2(14, 14), ImVec2(0, 1), ImVec2(1, 0), ImVec4(0, 0, 0, 0), tint)) {
					m_ShowLogInfo = !m_ShowLogInfo;
				}
			}
			else if (ImGui::SmallButton(m_ShowLogInfo ? "[I]" : "( )")) {
				m_ShowLogInfo = !m_ShowLogInfo;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Info (%d)", infoCount);
		}

		ImGui::SameLine();

		{
			unsigned int warnIcon = EditorIcons::Get("warning", 16);
			ImVec4 tint = m_ShowLogWarn ? ImVec4(1, 1, 1, 1) : ImVec4(0.4f, 0.4f, 0.4f, 0.5f);
			if (warnIcon) {
				if (ImGui::ImageButton("##FilterWarn",
					static_cast<ImTextureID>(static_cast<intptr_t>(warnIcon)),
					ImVec2(14, 14), ImVec2(0, 1), ImVec2(1, 0), ImVec4(0, 0, 0, 0), tint)) {
					m_ShowLogWarn = !m_ShowLogWarn;
				}
			}
			else {
				ImGui::PushStyleColor(ImGuiCol_Text, tint);
				if (ImGui::SmallButton(m_ShowLogWarn ? "W" : "(W)")) {
					m_ShowLogWarn = !m_ShowLogWarn;
				}
				ImGui::PopStyleColor();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Warnings (%d)", warnCount);
		}

		ImGui::SameLine();

		{
			unsigned int errIcon = EditorIcons::Get("error", 16);
			ImVec4 tint = m_ShowLogError ? ImVec4(1, 1, 1, 1) : ImVec4(0.4f, 0.4f, 0.4f, 0.5f);
			if (errIcon) {
				if (ImGui::ImageButton("##FilterError",
					static_cast<ImTextureID>(static_cast<intptr_t>(errIcon)),
					ImVec2(14, 14), ImVec2(0, 1), ImVec2(1, 0), ImVec4(0, 0, 0, 0), tint)) {
					m_ShowLogError = !m_ShowLogError;
				}
			}
			else {
				ImGui::PushStyleColor(ImGuiCol_Text, tint);
				if (ImGui::SmallButton(m_ShowLogError ? "E" : "(E)")) {
					m_ShowLogError = !m_ShowLogError;
				}
				ImGui::PopStyleColor();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Errors (%d)", errorCount);
		}

		ImGui::Separator();

		ImGui::BeginChild("##LogEntries", ImVec2(-1.0f, -1.0f), true,
			ImGuiWindowFlags_HorizontalScrollbar);

		const bool wasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
		for (const auto& entry : m_LogEntries) {
			if (!IsLogEntryVisible(entry.Level, m_ShowLogInfo, m_ShowLogWarn, m_ShowLogError)) {
				continue;
			}

			const std::string line = std::string(GetLogLevelPrefix(entry.Level)) + entry.Message;
			ImGui::PushStyleColor(ImGuiCol_Text, GetLogLevelColor(entry.Level));
			ImGui::TextWrapped("%s", line.c_str());
			ImGui::PopStyleColor();
		}

		if (wasAtBottom) {
			ImGui::SetScrollHereY(1.0f);
		}

		if (ImGui::BeginPopupContextWindow("##LogTextCtx")) {
			if (ImGui::MenuItem("Copy All Visible")) {
				std::string all;
				for (const auto& visibleEntry : m_LogEntries) {
					if (!IsLogEntryVisible(visibleEntry.Level, m_ShowLogInfo, m_ShowLogWarn, m_ShowLogError)) {
						continue;
					}
					all += std::string(GetLogLevelPrefix(visibleEntry.Level)) + visibleEntry.Message + "\n";
				}
				ImGui::SetClipboardText(all.c_str());
			}
			ImGui::EndPopup();
		}
		ImGui::EndChild();
		ImGui::End();
	}

	void ImGuiEditorLayer::RenderProjectPanel() {
		if (!m_AssetBrowserInitialized) {
			std::string assetsRoot;
			AxiomProject* project = ProjectManager::GetCurrentProject();
			if (project) assetsRoot = project->AssetsDirectory;
			else assetsRoot = Path::Combine(Path::ExecutableDir(), "Assets");

			if (!Directory::Exists(assetsRoot)) {
				Directory::Create(assetsRoot);
			}

			m_AssetBrowser.Initialize(assetsRoot);
			m_AssetBrowserInitialized = true;
		}

		m_AssetBrowser.Render();
		if (m_AssetBrowser.TakeSelectionActivated() && !m_AssetBrowser.GetSelectedPath().empty()) {
			ClearEntitySelection();
		}
	}

	void ImGuiEditorLayer::ExecuteBuild() {
		m_BuildStartTime = std::chrono::steady_clock::now();

		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (!project) { AIM_ERROR_TAG("Build", "No project loaded."); return; }

		m_BuildOutputDir = std::string(m_BuildOutputDirBuffer);
		if (m_BuildOutputDir.empty()) { AIM_ERROR_TAG("Build", "No output directory specified."); return; }

		AIM_INFO_TAG("Build", "Starting build for '{}'...", project->Name);

		Scene* active = SceneManager::Get().GetActiveScene();
		if (active && active->IsDirty()) {
			std::string scenePath = project->GetSceneFilePath(active->GetName());
			SceneSerializer::SaveToFile(*active, scenePath);
			project->LastOpenedScene = active->GetName();
			project->Save();
			AIM_INFO_TAG("Build", "Saved current scene.");
		}

		auto exeDir = std::filesystem::path(Path::ExecutableDir());
		auto outDir = std::filesystem::path(m_BuildOutputDir);
		const std::string buildConfiguration = AxiomProject::GetActiveBuildConfiguration();

		if (std::filesystem::exists(project->CsprojPath)) {
			AIM_INFO_TAG("Build", "Compiling C# scripts...");
			Process::Result buildResult = Process::Run({
				"dotnet",
				"build",
				project->CsprojPath,
				"-c", buildConfiguration,
				"--nologo",
				"-v", "q",
				"-p:DefineConstants=" + AxiomProject::BuildManagedDefineConstants("AXIOM_BUILD")
			});
			if (!buildResult.Succeeded()) {
				AIM_ERROR_TAG("Build", "C# script compilation failed (exit code {}).", buildResult.ExitCode);
				if (!buildResult.Output.empty()) {
					AIM_ERROR_TAG("Build", "{}", buildResult.Output);
				}
				return;
			}
			AIM_INFO_TAG("Build", "C# scripts compiled.");
		}

		// Rebuild native scripts with the project's chosen build profile so
		// the shipped DLL has AXIOM_BUILD_RELEASE / AXIOM_BUILD_DEVELOPMENT
		// matching the C# side. Editor hot-reload always builds DEVELOPMENT;
		// here we override via -DAXIOM_BUILD_PROFILE so a Release build
		// actually strips dev-only #ifdef'd code from native scripts.
		// Skipped when the project has no CMakeLists (no native scripts).
		const std::filesystem::path nativeProjectDir(project->NativeScriptsDir);
		const std::filesystem::path nativeCMakeLists = nativeProjectDir / "CMakeLists.txt";
		if (std::filesystem::exists(nativeCMakeLists)) {
			AIM_INFO_TAG("Build", "Compiling native scripts ({} profile)...",
				AxiomProject::BuildProfileToString(project->ActiveBuildProfile));
			const std::string profileCmakeArg = std::string("-DAXIOM_BUILD_PROFILE=")
				+ ((project->ActiveBuildProfile == AxiomProject::BuildProfile::Release) ? "RELEASE" : "DEVELOPMENT");
			const std::filesystem::path nativeBuildDir = nativeProjectDir / "build";
			// Configure (overrides editor's cached AXIOM_BUILD_PROFILE so the
			// next-frame editor hot-reload also picks up the explicit value).
			Process::Result cfg = Process::Run({
				"cmake",
				"-B", nativeBuildDir.string(),
				"-S", nativeProjectDir.string(),
				"-DCMAKE_BUILD_TYPE=" + buildConfiguration,
				profileCmakeArg
			}, nativeProjectDir.string());
			if (!cfg.Succeeded()) {
				AIM_WARN_TAG("Build",
					"Native script CMake configure failed (exit code {}); shipping the editor's last-built native DLL.",
					cfg.ExitCode);
			} else {
				Process::Result bld = Process::Run({
					"cmake", "--build", nativeBuildDir.string(),
					"--config", buildConfiguration
				}, nativeProjectDir.string());
				if (!bld.Succeeded()) {
					AIM_WARN_TAG("Build",
						"Native script build failed (exit code {}); shipping the editor's last-built native DLL.",
						bld.ExitCode);
					if (!bld.Output.empty()) AIM_WARN_TAG("Build", "{}", bld.Output);
				} else {
					AIM_INFO_TAG("Build", "Native scripts compiled.");
				}
			}
		}

		try {
			std::filesystem::create_directories(outDir);
		}
		catch (const std::exception& e) {
			AIM_ERROR_TAG("Build", "Failed to create output directory: {}", e.what());
			return;
		}

		auto copyFile = [&](const std::filesystem::path& src, const std::filesystem::path& dest, const std::string& name) {
			try {
				if (!std::filesystem::exists(src)) {
					AIM_WARN_TAG("Build", "{} not found at {}", name, src.string());
					return;
				}
				auto canonical = std::filesystem::canonical(src);
				if (NeedsCopy(canonical, dest)) {
					std::filesystem::create_directories(dest.parent_path());
					std::filesystem::copy_file(canonical, dest, std::filesystem::copy_options::overwrite_existing);
					AIM_INFO_TAG("Build", "Copied {}", name);
				}
			}
			catch (const std::exception& e) {
				AIM_WARN_TAG("Build", "Failed to copy {}: {}", name, e.what());
			}
		};

		const std::filesystem::path runtimeOutputDirectory = exeDir / ".." / "Axiom-Runtime";
		const std::string runtimeExecutableFilename = GetRuntimeExecutableFilename();
		const std::string outputExecutableStem = project->ExecutableName.empty()
			? project->Name : project->ExecutableName;
		copyFile(runtimeOutputDirectory / runtimeExecutableFilename,
			outDir / (outputExecutableStem + std::filesystem::path(runtimeExecutableFilename).extension().string()),
			"runtime executable");
		copyFile(runtimeOutputDirectory / GetEngineRuntimeFilename(),
			outDir / GetEngineRuntimeFilename(),
			GetEngineRuntimeFilename());

		// GLFW and Glad are SharedLibs (premake5.lua: one shared copy across
		// engine.dll + runtime.exe). Tracy is also shared, but only when the
		// engine was built without --no-profiler. The runtime postbuild stages
		// all of them next to Axiom-Runtime.exe, so we copy from there.
		for (std::string_view depName : { std::string_view{"GLFW"}, std::string_view{"Glad"} }) {
			const std::string filename = SharedLibraryFilename(depName);
			copyFile(runtimeOutputDirectory / filename, outDir / filename, filename);
		}
		{
			const std::string tracyFilename = SharedLibraryFilename("Tracy");
			const std::filesystem::path tracySource = runtimeOutputDirectory / tracyFilename;
			if (std::filesystem::exists(tracySource)) {
				copyFile(tracySource, outDir / tracyFilename, tracyFilename);
			}
		}

		auto scriptCoreDir = exeDir / ".." / "Axiom-ScriptCore";
		copyFile(scriptCoreDir / "Axiom-ScriptCore.dll", outDir / "Axiom-ScriptCore.dll", "Axiom-ScriptCore.dll");
		copyFile(scriptCoreDir / "Axiom-ScriptCore.runtimeconfig.json", outDir / "Axiom-ScriptCore.runtimeconfig.json", "ScriptCore config");
		copyFile(scriptCoreDir / "Axiom-ScriptCore.deps.json", outDir / "Axiom-ScriptCore.deps.json", "ScriptCore deps");
		{
			const std::string netHostFilename = GetNetHostRuntimeFilename();
			const std::filesystem::path netHostSource = exeDir / netHostFilename;
			if (std::filesystem::exists(netHostSource)) {
				copyFile(netHostSource, outDir / netHostFilename, netHostFilename);
			}
		}

		try {
			std::filesystem::copy_file(project->ProjectFilePath, outDir / "axiom-project.json",
				std::filesystem::copy_options::overwrite_existing);
		}
		catch (const std::exception& e) {
			AIM_WARN_TAG("Build", "Failed to copy axiom-project.json: {}", e.what());
		}

		if (std::filesystem::exists(project->AssetsDirectory)) {
			int updatedFiles = CopyDirIncremental(project->AssetsDirectory, outDir / "Assets");
			AIM_INFO_TAG("Build", "Assets: {} file(s) updated", updatedFiles);
		}

		{
			std::string axiomAssetsSrc;
			if (std::filesystem::exists(project->AxiomAssetsDirectory)) {
				axiomAssetsSrc = project->AxiomAssetsDirectory;
			}
			else {
				axiomAssetsSrc = Path::ResolveAxiomAssets("");
			}

			if (!axiomAssetsSrc.empty() && std::filesystem::exists(axiomAssetsSrc)) {
				// Editor-only assets (icons, gizmo art, file-type previews)
				// have no business in a shipped runtime — skip them. The
				// excluded path is relative to AxiomAssets/, so the
				// recursive copy walks past Textures/Editor entirely.
				const std::vector<std::filesystem::path> excluded{
					std::filesystem::path("Textures") / "Editor",
				};
				int updatedFiles = CopyDirIncrementalExcluding(
					axiomAssetsSrc, outDir / "AxiomAssets", excluded);
				AIM_INFO_TAG("Build", "AxiomAssets: {} file(s) updated", updatedFiles);
			}
			else {
				AIM_WARN_TAG("Build", "AxiomAssets not found - build may be incomplete");
			}
		}

		{
			auto userBinDir = std::filesystem::path(project->GetUserAssemblyOutputPath()).parent_path();
			if (std::filesystem::exists(userBinDir)) {
				auto destBinDir = outDir / "bin" / buildConfiguration;
				int copied = 0;
				for (const auto& entry : std::filesystem::directory_iterator(userBinDir)) {
					if (!entry.is_regular_file()) continue;
					auto ext = entry.path().extension().string();
					if (ext == ".dll" || ext == ".json") {
						copyFile(entry.path(), destBinDir / entry.path().filename(), entry.path().filename().string());
						copied++;
					}
				}
				AIM_INFO_TAG("Build", "User assemblies: {} file(s) copied", copied);
			}
		}

		{
			std::string nativeDll = project->GetNativeDllPath();
			if (std::filesystem::exists(nativeDll)) {
				const std::filesystem::path nativeLibraryPath(nativeDll);
				copyFile(nativeDll,
					outDir / "NativeScripts" / "build" / buildConfiguration / nativeLibraryPath.filename(),
					"native script DLL");
			}
		}

		float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_BuildStartTime).count();
		AIM_INFO_TAG("Build", "Build completed in {:.2f}s -> {}", elapsed, m_BuildOutputDir);

#ifdef AIM_PLATFORM_WINDOWS
		ShellExecuteA(nullptr, "open", outDir.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
	}

	void ImGuiEditorLayer::RenderBuildPanel() {
		if (!m_ShowBuildPanel) return;

		ImGui::Begin("Build", &m_ShowBuildPanel);

		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (project) {
			// Sync m_BuildSceneList with disk every frame so newly-imported
			// scenes auto-appear and deleted ones drop out, while preserving
			// any manual drag-drop reordering the user has done within the
			// panel. AssetRegistry caches the scan, so this is cheap when
			// nothing has changed since last sync.
			AssetRegistry::Sync();
			std::vector<std::string> diskScenes;
			diskScenes.reserve(m_BuildSceneList.size() + 4);
			for (const AssetRegistry::Record& record : AssetRegistry::FindAll(AssetKind::Scene)) {
				diskScenes.push_back(std::filesystem::path(record.Path).stem().string());
			}

			// Drop entries whose .scene file no longer exists.
			m_BuildSceneList.erase(
				std::remove_if(m_BuildSceneList.begin(), m_BuildSceneList.end(),
					[&](const std::string& s) {
						return std::find(diskScenes.begin(), diskScenes.end(), s) == diskScenes.end();
					}),
				m_BuildSceneList.end());

			// Append entries the user has not seen yet (new on disk).
			for (const std::string& stem : diskScenes) {
				if (std::find(m_BuildSceneList.begin(), m_BuildSceneList.end(), stem) == m_BuildSceneList.end()) {
					m_BuildSceneList.push_back(stem);
				}
			}

			// Pin StartupScene to position [0] so the [Startup] tag and
			// the runtime's first-loaded scene stay in sync with the
			// project file's StartupScene field.
			if (!project->StartupScene.empty()) {
				auto it = std::find(m_BuildSceneList.begin(), m_BuildSceneList.end(), project->StartupScene);
				if (it != m_BuildSceneList.end() && it != m_BuildSceneList.begin()) {
					std::string startupScene = *it;
					m_BuildSceneList.erase(it);
					m_BuildSceneList.insert(m_BuildSceneList.begin(), startupScene);
				}
			}

			if (ImGui::CollapsingHeader("Scene List", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Indent(8);
				if (m_BuildSceneList.empty()) {
					ImGui::TextDisabled("No scenes found in Assets/");
				}
				else {
					for (int i = 0; i < static_cast<int>(m_BuildSceneList.size()); i++) {
						ImGui::PushID(i);
						bool isStartup = (i == 0);

						if (isStartup) ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "[Startup]");
						else ImGui::TextDisabled("[%d]", i);

						ImGui::SameLine();
						const std::string sceneItemId = std::to_string(i);
						ImGuiUtils::SelectableEllipsis(m_BuildSceneList[i], sceneItemId.c_str());

						if (ImGui::BeginDragDropSource()) {
							ImGui::SetDragDropPayload("SCENE_LIST_ITEM", &i, sizeof(int));
							ImGui::Text("Move: %s", m_BuildSceneList[i].c_str());
							ImGui::EndDragDropSource();
						}
						if (ImGui::BeginDragDropTarget()) {
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_LIST_ITEM")) {
								int srcIndex = *static_cast<const int*>(payload->Data);
								if (srcIndex != i) {
									std::string moved = m_BuildSceneList[srcIndex];
									m_BuildSceneList.erase(m_BuildSceneList.begin() + srcIndex);
									int insertAt = (srcIndex < i) ? i - 1 : i;
									m_BuildSceneList.insert(m_BuildSceneList.begin() + insertAt, moved);
									if (!m_BuildSceneList.empty()) {
										project->StartupScene = m_BuildSceneList[0];
										project->Save();
									}
								}
							}
							ImGui::EndDragDropTarget();
						}

						ImGui::PopID();
					}
				}

				if (ImGui::BeginDragDropTarget()) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
						std::string droppedPath(static_cast<const char*>(payload->Data));
						if (std::filesystem::path(droppedPath).extension() == ".scene") {
							std::string sceneName = std::filesystem::path(droppedPath).stem().string();
							auto it = std::find(m_BuildSceneList.begin(), m_BuildSceneList.end(), sceneName);
							if (it == m_BuildSceneList.end()) {
								m_BuildSceneList.push_back(sceneName);
							}
						}
					}
					ImGui::EndDragDropTarget();
				}

				ImGui::Unindent(8);
			}

			ImGui::Spacing();

			if (ImGui::CollapsingHeader("Build Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Indent(8);
				if (ImGui::Button("Open Player Settings")) {
					m_ShowPlayerSettings = true;
				}

				ImGui::Spacing();

				// Build Profile dropdown. Drives the AXIOM_BUILD_DEVELOPMENT
				// vs AXIOM_BUILD_RELEASE compile-time defines passed to BOTH
				// C# and native scripts at build time. Switching the profile
				// also flips the runtime overlay defaults (stats + logs)
				// since release ships shouldn't expose dev diagnostics by
				// default — the user can re-enable explicitly via Player
				// Settings after the auto-flip.
				ImGui::TextUnformatted("Build Profile:");
				ImGui::SetNextItemWidth(-1);
				const char* profileItems[] = { "Development", "Release" };
				int profileIdx = (project->ActiveBuildProfile == AxiomProject::BuildProfile::Release) ? 1 : 0;
				if (ImGui::Combo("##BuildProfile", &profileIdx, profileItems, IM_ARRAYSIZE(profileItems))) {
					const auto newProfile = (profileIdx == 1)
						? AxiomProject::BuildProfile::Release
						: AxiomProject::BuildProfile::Development;
					if (newProfile != project->ActiveBuildProfile) {
						project->ActiveBuildProfile = newProfile;
						// Auto-flip overlay defaults to match the new profile.
						// Release = both off (ship-clean). Development = both on.
						const bool isDev = (newProfile == AxiomProject::BuildProfile::Development);
						project->ShowRuntimeStats = isDev;
						project->ShowRuntimeLogs  = isDev;
						project->Save();
					}
				}

				ImGui::Spacing();

				// Custom defines list. Symbols here are baked into both the
				// C# .csproj's <DefineConstants> and the native scripts'
				// CMakeLists target_compile_definitions on next compile.
				// Names only (no `=value`) — Unity-style scripting symbols.
				ImGui::TextUnformatted("Custom Defines:");
				// Reserve room for the "Add" button on the right so the
				// input field doesn't push it off-panel — previously the
				// input was width=-1 which left the button visually clipped
				// to a few pixels and made the click target unusable.
				const float addBtnWidth = ImGui::CalcTextSize("Add").x
					+ ImGui::GetStyle().FramePadding.x * 2.0f;
				const float availForInput = ImGui::GetContentRegionAvail().x
					- addBtnWidth - ImGui::GetStyle().ItemInnerSpacing.x;
				ImGui::SetNextItemWidth(availForInput);
				const bool entryEnter = ImGui::InputTextWithHint("##NewCustomDefine",
					"Add a symbol (e.g. STEAM_BUILD)…",
					m_CustomDefineEntryBuffer, sizeof(m_CustomDefineEntryBuffer),
					ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
				bool addClicked = ImGui::Button("Add##CustomDefine") || entryEnter;
				const bool entryReady = m_CustomDefineEntryBuffer[0] != '\0';
				if (addClicked && entryReady) {
					std::string newDef(m_CustomDefineEntryBuffer);
					// Trim whitespace + reject duplicates / empty strings.
					while (!newDef.empty() && std::isspace(static_cast<unsigned char>(newDef.back()))) newDef.pop_back();
					std::size_t firstNonSpace = 0;
					while (firstNonSpace < newDef.size()
						&& std::isspace(static_cast<unsigned char>(newDef[firstNonSpace]))) ++firstNonSpace;
					newDef.erase(0, firstNonSpace);
					if (!newDef.empty()
						&& std::find(project->CustomDefines.begin(), project->CustomDefines.end(), newDef) == project->CustomDefines.end()) {
						project->CustomDefines.push_back(std::move(newDef));
						project->Save();
					}
					m_CustomDefineEntryBuffer[0] = '\0';
				}

				if (project->CustomDefines.empty()) {
					ImGui::TextDisabled("(no custom defines)");
				} else {
					int removeIdx = -1;
					for (int i = 0; i < static_cast<int>(project->CustomDefines.size()); ++i) {
						ImGui::PushID(i);
						ImGui::Bullet();
						ImGui::TextUnformatted(project->CustomDefines[i].c_str());
						ImGui::SameLine();
						if (ImGui::SmallButton("x")) removeIdx = i;
						ImGui::PopID();
					}
					if (removeIdx >= 0) {
						project->CustomDefines.erase(project->CustomDefines.begin() + removeIdx);
						project->Save();
					}
				}

				ImGui::Spacing();
				ImGui::Text("Output Directory:");
				ImGui::SetNextItemWidth(-1);
				ImGui::InputText("##BuildOutputDir", m_BuildOutputDirBuffer, sizeof(m_BuildOutputDirBuffer));
				ImGui::Unindent(8);
			}
		}

		ImGui::Spacing();

		bool canBuild = project && !Application::GetIsPlaying() && m_BuildState == 0;
		if (!canBuild) ImGui::BeginDisabled();

		float halfWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		if (ImGui::Button("Build", ImVec2(halfWidth, 0))) {
			m_BuildState = 1;
			m_BuildAndPlay = false;
			m_BuildStartTime = std::chrono::steady_clock::now();
		}
		ImGui::SameLine();
		if (ImGui::Button("Build and Play", ImVec2(-1, 0))) {
			m_BuildState = 1;
			m_BuildAndPlay = true;
			m_BuildStartTime = std::chrono::steady_clock::now();
		}

		if (!canBuild) ImGui::EndDisabled();
		ImGui::End();
	}

	void ImGuiEditorLayer::RenderPlayerSettingsPanel() {
		if (!m_ShowPlayerSettings) return;

		ImGui::Begin("Project Settings", &m_ShowPlayerSettings);
		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			ImGui::TextDisabled("No project loaded");
			ImGui::End();
			return;
		}

		bool changed = false;
		if (ImGui::CollapsingHeader("Resolution", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::InputInt("Width", &project->BuildWidth);
			changed |= ImGui::InputInt("Height", &project->BuildHeight);
			if (project->BuildWidth < 320) project->BuildWidth = 320;
			if (project->BuildHeight < 240) project->BuildHeight = 240;
			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("Window", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::Checkbox("Fullscreen", &project->BuildFullscreen);
			changed |= ImGui::Checkbox("Resizable", &project->BuildResizable);
			ImGui::Unindent(8);
		}

		// UI scaling — Canvas-Scaler-style. Reference resolution defines
		// the "pixel unit" the UI was authored in; the layout system
		// multiplies SizeDelta / AnchoredPosition / padding by
		// (curResolution / refResolution) so the same scene renders
		// proportionally on any window size. Defaults are pre-seeded to
		// match the build resolution so a freshly-created project's
		// Game View previews 1:1 unless the user opts in.
		if (ImGui::CollapsingHeader("UI Scaling", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::InputInt("Reference Width##UIRef", &project->UIReferenceWidth);
			changed |= ImGui::InputInt("Reference Height##UIRef", &project->UIReferenceHeight);
			if (project->UIReferenceWidth  < 1) project->UIReferenceWidth  = 1;
			if (project->UIReferenceHeight < 1) project->UIReferenceHeight = 1;
			changed |= ImGui::SliderFloat("Match Width / Height", &project->UIScaleMatch, 0.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"0 = scale UI by window WIDTH only.\n"
					"1 = scale UI by window HEIGHT only.\n"
					"0.5 = balanced (geometric mean).\n"
					"\n"
					"Move toward 1 if your UI hugs the top/bottom edges,\n"
					"toward 0 if it hugs the left/right edges.");
			}
			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::Checkbox("Show runtime stats overlay (F6)", &project->ShowRuntimeStats);
			changed |= ImGui::Checkbox("Show runtime log overlay (F7)", &project->ShowRuntimeLogs);
			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("Executable", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			ImGui::TextUnformatted("Output Name:");
			char exeBuf[256];
			std::string current = project->ExecutableName.empty() ? project->Name : project->ExecutableName;
			std::snprintf(exeBuf, sizeof(exeBuf), "%s", current.c_str());
			ImGui::SetNextItemWidth(-1);
			if (ImGui::InputText("##ExecutableName", exeBuf, sizeof(exeBuf))) {
				const std::string newName(exeBuf);
				project->ExecutableName = (newName == project->Name) ? "" : newName;
				changed = true;
			}
			ImGui::TextDisabled("Default: project name (\"%s\"). The platform extension is appended automatically.",
				project->Name.c_str());
			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("Splash Screen", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			constexpr const char* k_SplashPickerKey = "ProjectSettings.SplashImage";

			if (auto pending = ReferencePicker::ConsumeSelection(k_SplashPickerKey); pending) {
				const std::string raw = *pending;
				uint64_t pickedId = 0;
				try { pickedId = std::stoull(raw); } catch (...) { pickedId = 0; }
				if (pickedId == 0) {
					project->SplashScreen.ImagePath.clear();
					changed = true;
				}
				else {
					std::string absPath = AssetRegistry::ResolvePath(pickedId);
					if (!absPath.empty()) {
						std::filesystem::path absFs(absPath);
						std::filesystem::path assetsDir(project->AssetsDirectory);
						if (absFs.string().find(assetsDir.string()) == 0) {
							project->SplashScreen.ImagePath = std::filesystem::relative(absFs, assetsDir.parent_path()).string();
						}
						else {
							project->SplashScreen.ImagePath = absFs.filename().string();
						}
						changed = true;
					}
				}
			}

			changed |= ImGui::Checkbox("Enabled##Splash", &project->SplashScreen.Enabled);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("When off, the runtime loads the startup scene immediately with no splash.");
			}

			if (project->SplashScreen.Enabled) {
				ImGui::Spacing();
				changed |= ImGui::SliderFloat("Duration (s)##Splash",
					&project->SplashScreen.DurationSeconds, 0.5f, 10.0f, "%.2f");
				changed |= ImGui::SliderFloat("Fade In (s)##Splash",
					&project->SplashScreen.FadeInSeconds, 0.0f, 3.0f, "%.2f");
				changed |= ImGui::SliderFloat("Fade Out (s)##Splash",
					&project->SplashScreen.FadeOutSeconds, 0.0f, 3.0f, "%.2f");

				ImGui::Spacing();
				float bg[3] = {
					project->SplashScreen.BackgroundR,
					project->SplashScreen.BackgroundG,
					project->SplashScreen.BackgroundB,
				};
				if (ImGui::ColorEdit3("Background##Splash", bg)) {
					project->SplashScreen.BackgroundR = bg[0];
					project->SplashScreen.BackgroundG = bg[1];
					project->SplashScreen.BackgroundB = bg[2];
					changed = true;
				}

				ImGui::Spacing();
				ImGui::TextUnformatted("Image (optional, replaces default Axiom logo):");
				if (project->SplashScreen.ImagePath.empty()) {
					ImGui::TextDisabled("(default Axiom logo)");
				}
				else {
					ImGuiUtils::TextDisabledEllipsis(project->SplashScreen.ImagePath);
				}
				if (ImGui::Button("Browse...##SplashImage")) {
					ReferencePicker::OpenForFieldKey(k_SplashPickerKey, "Select Splash Image",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				if (!project->SplashScreen.ImagePath.empty()) {
					ImGui::SameLine();
					if (ImGui::Button("Clear##SplashImage")) {
						project->SplashScreen.ImagePath.clear();
						changed = true;
					}
				}

				ImGui::Spacing();
				ImGui::TextUnformatted("Custom Text (optional, replaces version + platform line):");
				char textBuf[256];
				std::snprintf(textBuf, sizeof(textBuf), "%s", project->SplashScreen.CustomText.c_str());
				ImGui::SetNextItemWidth(-1);
				if (ImGui::InputTextWithHint("##SplashText", "Leave empty for engine default",
					textBuf, sizeof(textBuf))) {
					project->SplashScreen.CustomText = textBuf;
					changed = true;
				}
			}
			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("App Icon", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);

			constexpr const char* k_AppIconPickerKey = "ProjectSettings.AppIcon";

			// AppIconPath is stored as a project-relative path (e.g.
			// "Assets/icon.png") so the project file stays portable, but
			// TextureManager::ResolveTexturePath only searches CWD,
			// engine AxiomAssets, and <exe>/Assets/Textures — none of
			// which match a path-relative-to-project-root once the editor
			// is launched from somewhere other than the project dir.
			// Prepending project->RootDirectory turns it into a path the
			// `File::Exists(rawPath)` first branch of ResolveTexturePath
			// accepts.
			auto resolveProjectRelative = [&](const std::string& rel) -> std::string {
				std::filesystem::path p(rel);
				if (p.is_absolute()) return rel;
				return (std::filesystem::path(project->RootDirectory) / p).string();
			};

			// Apply a pending picker selection from a previous frame.
			if (auto pending = ReferencePicker::ConsumeSelection(k_AppIconPickerKey); pending) {
				const std::string raw = *pending;
				uint64_t pickedId = 0;
				try { pickedId = std::stoull(raw); } catch (...) { pickedId = 0; }
				if (pickedId == 0) {
					project->AppIconPath.clear();
					changed = true;
					Application::GetInstance()->GetWindow()->SetWindowIconFromResource();
				}
				else {
					std::string absPath = AssetRegistry::ResolvePath(pickedId);
					if (!absPath.empty()) {
						std::filesystem::path absFs(absPath);
						std::filesystem::path assetsDir(project->AssetsDirectory);
						if (absFs.string().find(assetsDir.string()) == 0) {
							project->AppIconPath = std::filesystem::relative(absFs, assetsDir.parent_path()).string();
						}
						else {
							project->AppIconPath = absFs.filename().string();
						}
						changed = true;
						TextureHandle icon = TextureManager::LoadTexture(resolveProjectRelative(project->AppIconPath));
						Texture2D* tex = TextureManager::GetTexture(icon);
						if (tex && tex->IsValid()) {
							Application::GetInstance()->GetWindow()->SetWindowIcon(tex);
						}
					}
				}
			}

			if (!project->AppIconPath.empty()) {
				TextureHandle iconHandle = TextureManager::LoadTexture(resolveProjectRelative(project->AppIconPath));
				Texture2D* iconTex = TextureManager::GetTexture(iconHandle);
				if (iconTex && iconTex->IsValid()) {
					ImGui::Image(
						static_cast<ImTextureID>(static_cast<intptr_t>(iconTex->GetHandle())),
						ImVec2(64, 64), ImVec2(0, 1), ImVec2(1, 0));
					ImGui::SameLine();
				}
				else {
					ImGui::TextDisabled("Failed to load:");
					ImGuiUtils::TextDisabledEllipsis(project->AppIconPath);
				}

				ImGui::BeginGroup();
				if (ImGui::Button("Browse...##AppIcon")) {
					ReferencePicker::OpenForFieldKey(k_AppIconPickerKey, "Select App Icon",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				ImGui::SameLine();
				if (ImGui::Button("Clear##AppIcon")) {
					project->AppIconPath.clear();
					changed = true;
					Application::GetInstance()->GetWindow()->SetWindowIconFromResource();
				}
				ImGui::EndGroup();

				if (iconTex && iconTex->IsValid()) {
					ImGuiUtils::TextDisabledEllipsis(project->AppIconPath);
				}
			}
			else {
				ImGui::TextDisabled("No icon set");
				if (ImGui::Button("Browse...##AppIcon")) {
					ReferencePicker::OpenForFieldKey(k_AppIconPickerKey, "Select App Icon",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				ImGui::SameLine();
				ImGui::TextDisabled("or drag an image from the Asset Browser");
			}

			if (ImGui::BeginDragDropTarget()) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
					std::string droppedPath(static_cast<const char*>(payload->Data));
					std::string ext = std::filesystem::path(droppedPath).extension().string();
					std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

					if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") {
						std::filesystem::path absPath(droppedPath);
						std::filesystem::path assetsDir(project->AssetsDirectory);
						if (absPath.string().find(assetsDir.string()) == 0) {
							project->AppIconPath = std::filesystem::relative(absPath, assetsDir.parent_path()).string();
						}
						else {
							project->AppIconPath = absPath.filename().string();
						}
						changed = true;

						TextureHandle icon = TextureManager::LoadTexture(resolveProjectRelative(project->AppIconPath));
						Texture2D* tex = TextureManager::GetTexture(icon);
						if (tex && tex->IsValid()) {
							Application::GetInstance()->GetWindow()->SetWindowIcon(tex);
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("Global Systems", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##GlobalSystemSearch", "Search global systems...",
				m_GlobalSystemSearchBuffer, sizeof(m_GlobalSystemSearchBuffer));

			std::string filter(m_GlobalSystemSearchBuffer);
			std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

			std::vector<EditorScriptDiscovery::ScriptEntry> scriptEntries;
			EditorScriptDiscovery::CollectProjectScriptEntries(scriptEntries);

			std::unordered_set<std::string> discoveredGlobalSystems;
			for (const auto& scriptEntry : scriptEntries) {
				if (!scriptEntry.IsGlobalSystem) {
					continue;
				}

				discoveredGlobalSystems.insert(scriptEntry.ClassName);
				if (!filter.empty()) {
					std::string lowerClassName = EditorScriptDiscovery::ToLowerCopy(scriptEntry.ClassName);
					std::string lowerPath = EditorScriptDiscovery::ToLowerCopy(scriptEntry.Path.string());
					if (lowerClassName.find(filter) == std::string::npos
						&& lowerPath.find(filter) == std::string::npos) {
						continue;
					}
				}

				auto it = std::find_if(project->GlobalSystems.begin(), project->GlobalSystems.end(),
					[&](const AxiomProject::GlobalSystemRegistration& registration) {
						return registration.ClassName == scriptEntry.ClassName;
					});
				bool active = it != project->GlobalSystems.end() ? it->Active : false;
				if (ImGui::Checkbox(scriptEntry.ClassName.c_str(), &active)) {
					if (it == project->GlobalSystems.end()) {
						project->GlobalSystems.push_back({ scriptEntry.ClassName, active });
					}
					else {
						it->Active = active;
					}
					changed = true;
				}
			}

			for (auto& registration : project->GlobalSystems) {
				if (discoveredGlobalSystems.contains(registration.ClassName)) {
					continue;
				}
				bool active = registration.Active;
				std::string label = registration.ClassName + " (missing)";
				if (ImGui::Checkbox(label.c_str(), &active)) {
					registration.Active = active;
					changed = true;
				}
			}

			ImGui::Unindent(8);
		}

		if (changed) {
			project->Save();
			std::vector<std::string> activeGlobalSystems;
			for (const auto& registration : project->GlobalSystems) {
				if (registration.Active && !registration.ClassName.empty()) {
					activeGlobalSystems.push_back(registration.ClassName);
				}
			}
			ScriptEngine::ShutdownGlobalSystems();
			ScriptEngine::InitializeGlobalSystems(activeGlobalSystems);
		}

		// Render any reference picker popup opened from this panel
		// (App Icon, Splash image). Without this, OpenForFieldKey
		// stages a request that never gets a render call and the popup
		// never appears for fields hosted outside the inspector.
		ReferencePicker::RenderPopup();

		ImGui::End();
	}

	void ImGuiEditorLayer::RenderAssetInspector() {
		const std::string& selectedPath = m_AssetBrowser.GetSelectedPath();
		if (selectedPath.empty()) {
			// Selection cleared. If the prefab inspector held a dirty prefab,
			// the save/discard prompt is driven from the dispatch above; here
			// just close cleanly.
			if (m_PrefabInspector.IsOpen() && !m_PrefabInspector.HasUnsavedChanges()) {
				m_PrefabInspector.Close();
				m_PrefabInspectorPath.clear();
			}
			ImGui::TextDisabled("No entity or asset selected");
			return;
		}

		std::filesystem::path path(selectedPath);
		if (!std::filesystem::exists(path)) {
			ImGui::TextDisabled("No entity or asset selected");
			return;
		}

		std::string name = path.filename().string();
		std::string ext = path.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		// `.prefab` selection routes through PrefabInspector. Selection change
		// while a dirty prefab is open opens a save/discard modal — the new
		// selection is queued in m_PendingPrefabSwitchPath until the user picks.
		if (ext == ".prefab") {
			if (m_PrefabInspector.IsOpen() && m_PrefabInspectorPath != selectedPath) {
				if (m_PrefabInspector.HasUnsavedChanges()) {
					m_PendingPrefabSwitchPath = selectedPath;
					m_ShowPrefabSavePrompt = true;
				}
				else {
					m_PrefabInspector.Open(selectedPath);
					m_PrefabInspectorPath = selectedPath;
				}
			}
			else if (!m_PrefabInspector.IsOpen()) {
				m_PrefabInspector.Open(selectedPath);
				m_PrefabInspectorPath = selectedPath;
			}
			m_PrefabInspector.Render();
			return;
		}

		// Non-prefab asset: clean up any open prefab inspector (no prompt — user
		// already navigated away; if they had unsaved work the prompt UX would
		// have caught it during the .prefab→.prefab switch above).
		if (m_PrefabInspector.IsOpen() && !m_PrefabInspector.HasUnsavedChanges()) {
			m_PrefabInspector.Close();
			m_PrefabInspectorPath.clear();
		}

		ImGui::TextDisabled("Asset:");
		ImGui::SameLine();
		ImGuiUtils::TextEllipsis(name);
		ImGui::Separator();

		ImGui::TextDisabled("Path:");
		ImGui::SameLine();
		ImGuiUtils::TextDisabledEllipsis(selectedPath);

		try {
			auto fileSize = std::filesystem::file_size(path);
			if (fileSize >= 1024 * 1024) ImGui::TextDisabled("Size: %.2f MB", fileSize / (1024.0f * 1024.0f));
			else if (fileSize >= 1024) ImGui::TextDisabled("Size: %.1f KB", fileSize / 1024.0f);
			else ImGui::TextDisabled("Size: %llu bytes", fileSize);
		}
		catch (...) {
		}

		ImGui::TextDisabled("Type: %s", ext.c_str());
		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
			ImGui::Spacing();
			const Texture2D* tex = GetPreviewTexture(path);
			if (tex && tex->IsValid()) {
				ImGuiUtils::DrawTexturePreview(tex->GetHandle(), tex->GetWidth(), tex->GetHeight(), 128.0f);
				ImGui::Text("%.0f x %.0f", tex->GetWidth(), tex->GetHeight());
			}
		}
	}

	const Texture2D* ImGuiEditorLayer::GetPreviewTexture(const std::filesystem::path& path) {
		const std::string canonicalPath = NormalizePreviewTexturePath(path);
		if (canonicalPath.empty()) {
			return nullptr;
		}

		if (const auto it = m_PreviewTextureLookup.find(canonicalPath); it != m_PreviewTextureLookup.end()) {
			PreviewTextureEntry& entry = m_PreviewTextureCache[it->second];
			entry.LastTouchTick = ++m_PreviewTextureTick;
			return entry.Texture.get();
		}

		// TODO(perf): Synchronous decode on UI thread — large textures cause first-select hitches.
		// Convert to async-task pattern (see axiom-add-editor-panel skill).
		auto texture = std::make_unique<Texture2D>(canonicalPath.c_str(), Filter::Point, Wrap::Clamp, Wrap::Clamp);
		if (!texture->IsValid()) {
			return nullptr;
		}

		PreviewTextureEntry entry;
		entry.CanonicalPath = canonicalPath;
		entry.Texture = std::move(texture);
		entry.LastTouchTick = ++m_PreviewTextureTick;

		const size_t index = m_PreviewTextureCache.size();
		m_PreviewTextureLookup[canonicalPath] = index;
		m_PreviewTextureCache.push_back(std::move(entry));
		TrimPreviewTextureCache();
		return m_PreviewTextureCache.back().Texture.get();
	}

	void ImGuiEditorLayer::TrimPreviewTextureCache() {
		while (m_PreviewTextureCache.size() > kMaxPreviewTextures) {
			auto victimIt = std::min_element(
				m_PreviewTextureCache.begin(),
				m_PreviewTextureCache.end(),
				[](const PreviewTextureEntry& a, const PreviewTextureEntry& b) {
					return a.LastTouchTick < b.LastTouchTick;
				});
			if (victimIt == m_PreviewTextureCache.end()) {
				break;
			}

			m_PreviewTextureCache.erase(victimIt);
			m_PreviewTextureLookup.clear();
			for (size_t i = 0; i < m_PreviewTextureCache.size(); ++i) {
				m_PreviewTextureLookup[m_PreviewTextureCache[i].CanonicalPath] = i;
			}
		}
	}

	void ImGuiEditorLayer::ClearPreviewTextureCache() {
		m_PreviewTextureLookup.clear();
		m_PreviewTextureCache.clear();
		m_PreviewTextureTick = 0;
	}

	void ImGuiEditorLayer::RenderPackageManagerPanel() {
		if (!m_ShowPackageManager) return;

		if (!m_PackageManagerInitialized) {
			m_PackageManager.Initialize();

			if (m_PackageManager.IsReady()) {
				m_PackageManager.AddSource(std::make_unique<NuGetSource>(m_PackageManager.GetToolPath()));
				m_PackageManager.AddSource(std::make_unique<GitHubSource>(
					m_PackageManager.GetToolPath(),
					"https://raw.githubusercontent.com/Ben-Scr/axiom-packages/main/index.json",
					"Engine Packages"));
			}

			m_PackageManagerPanel.Initialize(&m_PackageManager);
			m_PackageManagerInitialized = true;
		}

		// Pre-seed a sensible first-open size and minimum constraints so a
		// freshly-docked panel isn't squashed to its title bar — without
		// this, ImGui's dock layout could collapse the panel's content
		// area to 0 px and the user only sees the tab strip.
		ImGui::SetNextWindowSize(ImVec2(880, 540), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(420, 320), ImVec2(FLT_MAX, FLT_MAX));
		ImGui::Begin("Package Manager", &m_ShowPackageManager);
		m_PackageManagerPanel.Render();
		ImGui::End();
	}

} // namespace Axiom
