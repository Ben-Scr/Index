#include <pch.hpp>
#include "Gui/AssetBrowser.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Scripting/DataAssetManager.hpp"
#include "Core/Log.hpp"
#include "Core/Platform/Win32BuildProgressWindow.hpp"
#include "Editor/EditorPreferences.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Gui/ImGuiImplWebGPU.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptDiscovery.hpp"
#include "Serialization/Directory.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/SceneSerializerShared.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>

#ifdef IDX_PLATFORM_WINDOWS
#include <windows.h>
#include <shellapi.h>
#endif

namespace Index {

#ifdef IDX_PLATFORM_WINDOWS
	namespace {
		// Tracked (not detached) so AssetBrowser::Shutdown can join them before global destructors run.
		std::mutex s_ShellLaunchMutex;
		std::vector<std::thread> s_ShellLaunchThreads;

		void ReapFinishedShellThreads_Locked() {
			s_ShellLaunchThreads.erase(
				std::remove_if(s_ShellLaunchThreads.begin(), s_ShellLaunchThreads.end(),
					[](std::thread& t) {
						if (!t.joinable()) return true;
						return false;
					}),
				s_ShellLaunchThreads.end());
		}
	}

	void TrackShellLaunchThread(std::thread t) {
		std::scoped_lock lock(s_ShellLaunchMutex);
		ReapFinishedShellThreads_Locked();
		s_ShellLaunchThreads.push_back(std::move(t));
	}

	void JoinAllShellLaunchThreads() {
		std::vector<std::thread> drained;
		{
			std::scoped_lock lock(s_ShellLaunchMutex);
			drained.swap(s_ShellLaunchThreads);
		}
		for (auto& t : drained) {
			if (t.joinable()) t.join();
		}
	}
#endif // IDX_PLATFORM_WINDOWS

	namespace {
		bool IsNativeScriptSourceExtension(std::string extension) {
			std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			return extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx"
				|| extension == ".h" || extension == ".hh" || extension == ".hpp" || extension == ".hxx"
				|| extension == ".inl" || extension == ".ipp";
		}

		std::filesystem::path GetNativeSourceDirectory(bool ensureProjectFiles = false) {
			if (IndexProject* project = ProjectManager::GetCurrentProject()) {
				if (ensureProjectFiles) {
					project->EnsureNativeScriptProjectFiles();
				}
				return project->NativeSourceDir;
			}

			return std::filesystem::path(Path::ExecutableDir()) / ".." / ".." / ".." / "Index-NativeScripts" / "Source";
		}

		std::filesystem::path ResolveNativeScriptMirrorPath(const std::string& path, bool ensureProjectFiles = false) {
			const std::filesystem::path sourcePath(path);
			if (!IsNativeScriptSourceExtension(sourcePath.extension().string())) {
				return {};
			}

			std::filesystem::path nativeSourceDir = GetNativeSourceDirectory(ensureProjectFiles);
			if (nativeSourceDir.empty()) {
				return {};
			}

			const std::filesystem::path mirrorPath = nativeSourceDir / sourcePath.filename();
			std::error_code ec;
			if (std::filesystem::equivalent(sourcePath, mirrorPath, ec)) {
				return {};
			}

			return mirrorPath;
		}

		IndexProject::EditorEntityNameSuffixStyle GetAssetDuplicateSuffixStyle()
		{
			if (IndexProject* project = ProjectManager::GetCurrentProject()) {
				return project->EditorAssetDuplicateSuffix;
			}
			return IndexProject::EditorEntityNameSuffixStyle::ParenthesizedNumber;
		}

		std::string FormatDuplicateAssetName(const std::string& baseName, int index)
		{
			switch (GetAssetDuplicateSuffixStyle()) {
			case IndexProject::EditorEntityNameSuffixStyle::SpaceNumber:
				return baseName + " " + std::to_string(index);
			case IndexProject::EditorEntityNameSuffixStyle::HyphenNumber:
				return baseName + "-" + std::to_string(index);
			case IndexProject::EditorEntityNameSuffixStyle::UnderscoreNumber:
				return baseName + "_" + std::to_string(index);
			case IndexProject::EditorEntityNameSuffixStyle::ParenthesizedNumber:
			default:
				return baseName + " (" + std::to_string(index) + ")";
			}
		}

		// .scene files embed "name"; if not synced after rename, the hierarchy still shows the old stem. Handles both JSON and Binary formats.
		void SyncSceneEmbeddedNameToFilename(const std::string& scenePath, const std::string& newStem) {
			Json::Value root;
			std::string readError;
			if (!SceneSerializerStorage::ReadRootFromFile(scenePath, root, &readError) || !root.IsObject()) {
				IDX_WARN_TAG("AssetBrowser",
					"Renamed scene '{}' could not be read for embedded-name sync ({}); the embedded name was not updated.",
					scenePath, readError);
				return;
			}

			if (Json::Value* nameNode = root.FindMember("name")) {
				if (nameNode->AsStringOr() == newStem) {
					return;
				}
				*nameNode = Json::Value(newStem);
			}
			else {
				root.AddMember("name", Json::Value(newStem));
			}

			// Preserve the on-disk format. A binary scene rewritten as JSON
			// would silently switch formats and break any tooling that
			// checks the magic header.
			const std::vector<std::uint8_t> rawBytes = File::ReadAllBytes(scenePath);
			const SceneSerializationFormat format = SceneSerializerStorage::IsBinaryData(rawBytes)
				? SceneSerializationFormat::Binary
				: SceneSerializationFormat::Json;

			if (!SceneSerializerStorage::WriteRootToFile(scenePath, root, format)) {
				IDX_WARN_TAG("AssetBrowser",
					"Failed to write renamed scene '{}' back to disk; embedded name remains stale.",
					scenePath);
			}
		}

		void UpdateLoadedSceneNameAfterRename(const std::string& oldStem, const std::string& newStem) {
			SceneManager::Get().ForeachLoadedScene([&](Scene& scene) {
				if (scene.GetName() == oldStem) {
					scene.SetName(newStem);
				}
			});

			if (IndexProject* project = ProjectManager::GetCurrentProject()) {
				// Scenes are referenced BY NAME throughout the project file. A rename
				// must remap EVERY reference, not just LastOpenedScene — otherwise
				// StartupScene / BuildSceneList keep pointing at the old stem and a
				// build's runtime loads a now-missing scene (blank window) even though
				// the renamed .scene file ships fine.
				bool dirty = false;
				if (project->LastOpenedScene == oldStem) {
					project->LastOpenedScene = newStem;
					dirty = true;
				}
				if (project->StartupScene == oldStem) {
					project->StartupScene = newStem;
					dirty = true;
				}
				for (std::string& sceneName : project->BuildSceneList) {
					if (sceneName == oldStem) {
						sceneName = newStem;
						dirty = true;
					}
				}
				if (dirty) {
					project->Save();
				}
			}
		}

		std::uintmax_t SafeFileSize(const std::filesystem::path& path) {
			std::error_code ec;
			const std::uintmax_t size = std::filesystem::file_size(path, ec);
			return ec ? 0 : size;
		}

		void CollectImportByteSize(const std::filesystem::path& path, std::uintmax_t& totalBytes) {
			std::error_code ec;
			if (std::filesystem::is_regular_file(path, ec) && !ec) {
				totalBytes += SafeFileSize(path);
				return;
			}

			ec.clear();
			if (!std::filesystem::is_directory(path, ec) || ec) {
				return;
			}

			for (std::filesystem::recursive_directory_iterator it(
				 path,
				 std::filesystem::directory_options::skip_permission_denied,
				 ec), end;
				 it != end;
				 it.increment(ec)) {
				if (ec) {
					ec.clear();
					continue;
				}

				std::error_code fileEc;
				if (it->is_regular_file(fileEc) && !fileEc) {
					totalBytes += SafeFileSize(it->path());
				}
			}
		}

		std::filesystem::path MakeUniqueImportFilePath(
			const std::filesystem::path& source,
			const std::filesystem::path& destinationDirectory)
		{
			std::filesystem::path dest = destinationDirectory / source.filename();
			std::error_code ec;
			if (!std::filesystem::exists(dest, ec)) {
				return dest;
			}

			const std::string stem = dest.stem().string();
			const std::string ext = dest.extension().string();
			for (int counter = 1; counter < 10000; ++counter) {
				dest = destinationDirectory / (stem + " (" + std::to_string(counter) + ")" + ext);
				ec.clear();
				if (!std::filesystem::exists(dest, ec)) {
					return dest;
				}
			}

			return destinationDirectory / (stem + " (" + std::to_string(std::time(nullptr)) + ")" + ext);
		}

		void AddImportedAssetPath(std::vector<std::string>& paths, const std::filesystem::path& path) {
			const std::string assetPath = path.string();
			if (!AssetRegistry::IsMetaFilePath(assetPath)) {
				paths.push_back(assetPath);
			}
		}

		void CollectRegistryAssetPaths(const std::filesystem::path& path, std::vector<std::string>& paths)
		{
			std::error_code ec;
			if (std::filesystem::is_regular_file(path, ec) && !ec) {
				AddImportedAssetPath(paths, path);
				return;
			}

			ec.clear();
			if (!std::filesystem::is_directory(path, ec) || ec) {
				return;
			}

			for (std::filesystem::recursive_directory_iterator it(
				 path,
				 std::filesystem::directory_options::skip_permission_denied,
				 ec), end;
				 it != end;
				 it.increment(ec))
			{
				if (ec) {
					ec.clear();
					continue;
				}

				std::error_code fileEc;
				if (!it->is_regular_file(fileEc) || fileEc) {
					continue;
				}

				AddImportedAssetPath(paths, it->path());
			}
		}

		void RegisterRegistryAssetTree(const std::filesystem::path& path)
		{
			std::vector<std::string> assetPaths;
			CollectRegistryAssetPaths(path, assetPaths);
			for (const std::string& assetPath : assetPaths) {
				AssetRegistry::ImportAssetPathIncremental(assetPath);
			}
		}

		void ForgetRegistryAssetPaths(const std::vector<std::string>& assetPaths)
		{
			for (const std::string& assetPath : assetPaths) {
				AssetRegistry::ForgetAssetPathIncremental(assetPath);
			}
		}

		bool CopyFileChunked(
			const std::filesystem::path& source,
			const std::filesystem::path& destination,
			bool skipExisting,
			const std::atomic_bool& keepRunning,
			const std::function<void(std::uintmax_t)>& onCopiedBytes)
		{
			std::error_code ec;
			if (skipExisting && std::filesystem::exists(destination, ec) && !ec) {
				return false;
			}

			if (destination.has_parent_path()) {
				std::filesystem::create_directories(destination.parent_path(), ec);
				if (ec) {
					return false;
				}
			}

			std::ifstream input(source, std::ios::binary);
			if (!input.is_open()) {
				return false;
			}

			std::ofstream output(destination, std::ios::binary | std::ios::trunc);
			if (!output.is_open()) {
				return false;
			}

			// Heap, not stack: a 1 MB std::array here overflowed the import worker
			// thread's 1 MB default stack the moment CopyFileChunked was entered,
			// crashing the editor on every drag-import. std::vector keeps the same
			// throughput-friendly chunk size without touching the stack guard page.
			std::vector<char> buffer(1024 * 1024);
			while (keepRunning.load(std::memory_order_acquire) && input) {
				input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const std::streamsize read = input.gcount();
				if (read <= 0) {
					break;
				}

				output.write(buffer.data(), read);
				if (!output) {
					return false;
				}

				onCopiedBytes(static_cast<std::uintmax_t>(read));
			}

			return keepRunning.load(std::memory_order_acquire) && !input.bad() && output.good();
		}
	}

	void AssetBrowser::BeginRename(const std::string& path, const std::string& currentName) {
		m_IsRenaming = true;
		m_RenamePath = path;
		m_PressedPath.clear();
		m_RenameFrameCounter = 0;

		// Strip extension from display name when ShowFileExtensions is off; CommitRename re-appends it on commit.
		const bool showExt = EditorPreferences::GetShowFileExtensions();
		std::string display = currentName;
		if (!showExt) {
			std::filesystem::path p(currentName);
			std::string stem = p.stem().string();
			std::string ext = p.extension().string();
			// stem.empty() catches dotfiles like ".env" — leave them
			// alone so we don't render an empty rename buffer.
			if (!ext.empty() && !stem.empty()) {
				display = stem;
			}
		}
		std::snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", display.c_str());
	}

	void AssetBrowser::CommitRename() {
		std::string newName(m_RenameBuffer);
		std::string oldName = std::filesystem::path(m_RenamePath).filename().string();

		if (m_PendingScriptType == PendingScriptType::EntityPrefab) {
			// Strip a trailing ".prefab" the user typed so we don't end up with "Foo.prefab.prefab".
			std::string baseName = newName;
			if (baseName.size() > 7) {
				std::string lower = baseName;
				std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
				if (lower.substr(lower.size() - 7) == ".prefab") {
					baseName = baseName.substr(0, baseName.size() - 7);
				}
			}
			if (baseName.empty()) {
				baseName = "NewPrefab";
			}

			// Suffix " (N)" to dodge collisions with anything already in the folder.
			std::filesystem::path targetPath = std::filesystem::path(m_PendingScriptDir) / (baseName + ".prefab");
			std::error_code existsEc;
			for (int n = 1; targetPath.string() != m_RenamePath
				&& std::filesystem::exists(targetPath, existsEc) && n < 10000; ++n) {
				targetPath = std::filesystem::path(m_PendingScriptDir) / (FormatDuplicateAssetName(baseName, n) + ".prefab");
				existsEc.clear();
			}

			const std::string finalPath = targetPath.string();
			bool renameSucceeded = true;
			if (finalPath != m_RenamePath) {
				std::error_code renameEc;
				std::filesystem::rename(m_RenamePath, finalPath, renameEc);
				if (renameEc) {
					renameSucceeded = false;
					IDX_WARN_TAG("AssetBrowser",
						"Failed to rename '{}' to '{}': {}",
						m_RenamePath, finalPath, renameEc.message());
				}
			}

			if (renameSucceeded) {
				m_SelectedPath = finalPath;
				if (finalPath != m_RenamePath) {
					AssetRegistry::MoveCompanionMetadata(m_RenamePath, finalPath);
				}
				else {
					AssetRegistry::ImportAssetPathIncremental(finalPath);
				}
			}
			m_PendingScriptType = PendingScriptType::None;
			m_PendingScriptDir.clear();
			m_PendingPrefabSourceEntity = entt::null;
			m_NeedsRefresh = true;
			CancelRename();
			return;
		}

		if (m_PendingScriptType != PendingScriptType::None) {
			std::string className = newName;
			const bool isCSharp = m_PendingScriptType == PendingScriptType::CSharp
				|| m_PendingScriptType == PendingScriptType::CSharpEmpty
				|| m_PendingScriptType == PendingScriptType::CSharpComponent
				|| m_PendingScriptType == PendingScriptType::CSharpNativeComponent
				|| m_PendingScriptType == PendingScriptType::CSharpSceneSystem
				|| m_PendingScriptType == PendingScriptType::CSharpGlobalSystem
				|| m_PendingScriptType == PendingScriptType::CSharpDataAsset;

			const bool isComponent = m_PendingScriptType == PendingScriptType::CSharpComponent;
			const bool isNativeComponent = m_PendingScriptType == PendingScriptType::CSharpNativeComponent;
			const bool isSceneSystem = m_PendingScriptType == PendingScriptType::CSharpSceneSystem;
			const bool isGlobalSystem = m_PendingScriptType == PendingScriptType::CSharpGlobalSystem;
			const bool isEmpty = m_PendingScriptType == PendingScriptType::CSharpEmpty;
			const bool isDataAsset = m_PendingScriptType == PendingScriptType::CSharpDataAsset;
			std::string ext = isCSharp ? ".cs" : (isComponent ? ".hpp" : ".cpp");

			if (!className.empty() && className.find('.') == std::string::npos) {
			}
			else if (className.size() > ext.size()) {
				std::string lowerName = className;
				std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
				if (lowerName.substr(lowerName.size() - ext.size()) == ext)
					className = className.substr(0, className.size() - ext.size());
			}

			const std::string classNameFallback = isComponent       ? "NewComponent"
			                                    : isNativeComponent ? "NewNativeComponent"
			                                    : isSceneSystem      ? "NewSceneSystem"
			                                    : isGlobalSystem    ? "NewGlobalSystem"
			                                    : isDataAsset       ? "NewDataAsset"
			                                                        : "NewScript";
			className = EditorScriptDiscovery::SanitizeIdentifier(className, classNameFallback);

			std::string finalFileName = className + ext;
			std::string finalPath = (std::filesystem::path(m_PendingScriptDir) / finalFileName).string();

			if (std::filesystem::exists(m_RenamePath)) {
				std::error_code ec;
				std::filesystem::remove(m_RenamePath, ec);
			}

			if (isCSharp) {
				std::string boilerplate;
				if (isComponent) {
					boilerplate =
						"using Index;\n"
						"\n"
						"public class " + className + " : Component\n"
						"{\n"
						"     \n"
						"}\n";
				}
				else if (isNativeComponent) {
					boilerplate =
						"using System.Runtime.InteropServices;\n"
						"using Index;\n"
						"using Index.Components;\n"
						"\n"
						"[StructLayout(LayoutKind.Sequential)]\n"
						"public struct " + className + " : IComponent\n"
						"{\n"
						"     \n"
						"    public " + className + "() { }\n"
						"}\n";
				}
				else if (isSceneSystem) {
					boilerplate =
						"using Index;\n"
						"\n"
						"public class " + className + " : SceneSystem\n"
						"{\n"
						"    public override void OnStart()\n"
						"    {\n"
						"    }\n"
						"\n"
						"    public override void OnUpdate()\n"
						"    {\n"
						"    }\n"
						"}\n";
				}
				else if (isGlobalSystem) {
					boilerplate =
						"using Index;\n"
						"\n"
						"public class " + className + " : GlobalSystem\n"
						"{\n"
						"    public static " + className + " Instance { get; private set; } = null!;\n"
						"\n"
						"    public override void OnInitialize()\n"
						"    {\n"
						"        Instance = this;\n"
						"    }\n"
						"\n"
						"    public override void OnUpdate()\n"
						"    {\n"
						"    }\n"
						"}\n";
				}
				else if (isDataAsset) {
					boilerplate =
						"using Index;\n"
						"\n"
						"[CreateDataAsset(\"" + className + "\")]\n"
						"public class " + className + " : DataAsset\n"
						"{\n"
						"     \n"
						"}\n";
				}
				else if (isEmpty) {
					boilerplate =
						"using Index;\n"
						"\n"
						"public class " + className + "\n"
						"{\n"
						"     \n"
						"}\n";
				}
				else {
					boilerplate =
						"using Index;\n"
						"\n"
						"public class " + className + " : EntityScript\n"
						"{\n"
						"    public override void OnStart()\n"
						"    {\n"
						"    }\n"
						"\n"
						"    public override void OnUpdate()\n"
						"    {\n"
						"    }\n"
						"}\n";
				}

				std::ofstream file(finalPath);
				if (file.is_open()) { file << boilerplate; file.close(); }

				IndexProject* project = ProjectManager::GetCurrentProject();
				if (!project) {
					auto sandboxDir = std::filesystem::path(Path::ExecutableDir()) / ".." / ".." / ".." / "Index-Sandbox" / "Source";
					if (std::filesystem::exists(sandboxDir)) {
						auto dst = sandboxDir / finalFileName;
						if (!std::filesystem::exists(dst)) {
							std::ofstream f(dst); if (f.is_open()) { f << boilerplate; f.close(); }
						}
					}
				}
			}
			else {
				std::string boilerplate = isComponent
					? "#pragma once\n"
					  "\n"
					  "struct " + className + "\n"
					  "{\n"
					  "    float Value = 0.0f;\n"
					  "};\n"
					: "#include <Scripting/NativeScript.hpp>\n"
					  "\n"
					  "class " + className + " : public Index::NativeScript {\n"
					  "public:\n"
					  "    void Start() override\n"
					  "    {\n"
					  "    }\n"
					  "\n"
					  "    void Update(float dt) override\n"
					  "    {\n"
					  "    }\n"
					  "\n"
					  "    void OnDestroy() override\n"
					  "    {\n"
					  "    }\n"
					  "};\n"
					  "REGISTER_SCRIPT(" + className + ")\n";

				std::ofstream file(finalPath);
				if (file.is_open()) { file << boilerplate; file.close(); }

				std::filesystem::path nativeDir = GetNativeSourceDirectory(true);
				if (!nativeDir.empty()) {
					std::error_code ec;
					std::filesystem::create_directories(nativeDir, ec);
					auto dst = nativeDir / finalFileName;
					if (!std::filesystem::exists(dst)) {
						std::ofstream f(dst); if (f.is_open()) { f << boilerplate; f.close(); }
					}
				}
			}

			AssetRegistry::ImportAssetPathIncremental(finalPath);
			m_SelectedPath = finalPath;
			m_PendingScriptType = PendingScriptType::None;
			m_PendingScriptDir.clear();
			m_NeedsRefresh = true;
			CancelRename();
			return;
		}

		if (!newName.empty() && newName.find('.') == std::string::npos) {
			std::string oldExt = std::filesystem::path(m_RenamePath).extension().string();
			if (!oldExt.empty()) {
				newName += oldExt;
			}
		}

		if (!newName.empty() && newName != oldName) {
			RenameEntry(m_RenamePath, newName);
		}
		CancelRename();
	}

	void AssetBrowser::CancelRename() {
		if (m_PendingScriptType != PendingScriptType::None) {
			if (!m_RenamePath.empty() && std::filesystem::exists(m_RenamePath)) {
				std::error_code ec;
				std::filesystem::remove(m_RenamePath, ec);
				if (!ec) {
					AssetRegistry::DeleteCompanionMetadata(m_RenamePath);
				}
			}
			m_PendingScriptType = PendingScriptType::None;
			m_PendingScriptDir.clear();
			m_PendingPrefabSourceEntity = entt::null;
			m_NeedsRefresh = true;
		}

		m_IsRenaming = false;
		m_RenamePath.clear();
		m_RenameFrameCounter = 0;
	}

	bool AssetBrowser::IsRenamingEntry(const std::string& path) const {
		return m_IsRenaming && m_RenamePath == path;
	}

	bool AssetBrowser::BeginRenameSelected() {
		if (m_SelectedPath.empty() || m_IsRenaming) {
			return false;
		}

		std::error_code ec;
		if (!std::filesystem::exists(m_SelectedPath, ec) || ec) {
			return false;
		}

		BeginRename(m_SelectedPath, std::filesystem::path(m_SelectedPath).filename().string());
		return true;
	}

	void AssetBrowser::DeleteEntry(const std::string& path) {
		// Defer thumbnail invalidation: see m_PendingThumbnailInvalidates docs.
		// The file itself is deleted now so on-disk state is consistent before
		// Render() returns; only the GPU-bound Texture2D destruction waits.
		m_PendingThumbnailInvalidates.push_back(path);
		const std::filesystem::path nativeMirrorPath = ResolveNativeScriptMirrorPath(path);
		const std::filesystem::path pathFs(path);
		const bool wasSceneFile = pathFs.extension() == ".scene";
		const std::string sceneStem = wasSceneFile ? pathFs.stem().string() : std::string{};
		std::vector<std::string> deletedAssetPaths;
		CollectRegistryAssetPaths(pathFs, deletedAssetPaths);

		if (Directory::Delete(path)) {
			ForgetRegistryAssetPaths(deletedAssetPaths);
			if (!nativeMirrorPath.empty()) {
				std::error_code ec;
				std::filesystem::remove(nativeMirrorPath, ec);
			}

			if (m_SelectedPath == path) {
				m_SelectedPath.clear();
			}
			m_SelectedPaths.erase(
				std::remove(m_SelectedPaths.begin(), m_SelectedPaths.end(), path),
				m_SelectedPaths.end());
			if (m_PressedPath == path) {
				m_PressedPath.clear();
			}
			CancelRename();
			m_NeedsRefresh = true;

			// Drop the SceneDefinition so a script-side LoadScene of the
			// deleted name can't resurrect an empty Scene from the registered
			// definition.
			if (wasSceneFile && !sceneStem.empty()) {
				SceneManager::Get().UnregisterScene(sceneStem);
			}
		}
	}

	void AssetBrowser::RenameEntry(const std::string& path, const std::string& newName) {
		// Same dangling-WGPUTextureView risk as DeleteEntry: ImGui may have
		// already recorded a draw command for the old-key thumbnail this
		// frame. Queue the invalidate for next frame instead.
		m_PendingThumbnailInvalidates.push_back(path);

		std::string oldExt = std::filesystem::path(path).extension().string();
		std::string oldStem = std::filesystem::path(path).stem().string();
		std::vector<std::string> renamedAssetPaths;
		CollectRegistryAssetPaths(path, renamedAssetPaths);

		if (Directory::Rename(path, newName)) {
			std::filesystem::path p(path);
			std::string newPath = (p.parent_path() / newName).string();
			ForgetRegistryAssetPaths(renamedAssetPaths);
			RegisterRegistryAssetTree(newPath);
			if (m_SelectedPath == path) {
				m_SelectedPath = newPath;
			}
			for (std::string& selectedPath : m_SelectedPaths) {
				if (selectedPath == path) {
					selectedPath = newPath;
				}
			}

			std::string ext = std::filesystem::path(newName).extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			std::string newStem = std::filesystem::path(newName).stem().string();
			const bool oldIsNativeScript = IsNativeScriptSourceExtension(oldExt);
			const bool newIsNativeScript = IsNativeScriptSourceExtension(ext);

			std::filesystem::path projectSourceDir;
			IndexProject* project = ProjectManager::GetCurrentProject();
			if (oldIsNativeScript || newIsNativeScript)
			{
				projectSourceDir = GetNativeSourceDirectory(newIsNativeScript);
			}
			else if (ext == ".cs")
			{
				if (project)
					projectSourceDir = project->ScriptsDirectory;
				else
					projectSourceDir = std::filesystem::path(Path::ExecutableDir()) / ".." / ".." / ".." / "Index-Sandbox" / "Source";
			}

			if (!projectSourceDir.empty() && (std::filesystem::exists(projectSourceDir) || newIsNativeScript))
			{
				if (newIsNativeScript) {
					std::error_code ec;
					std::filesystem::create_directories(projectSourceDir, ec);
				}

				auto oldProjectFile = projectSourceDir / (oldStem + oldExt);
				auto newProjectFile = projectSourceDir / newName;

				if (oldIsNativeScript && !newIsNativeScript)
				{
					std::error_code ec;
					std::filesystem::remove(oldProjectFile, ec);
				}
				else if (newIsNativeScript)
				{
					if (std::filesystem::exists(oldProjectFile))
					{
						std::error_code ec;
						std::filesystem::rename(oldProjectFile, newProjectFile, ec);
					}
					else if (std::filesystem::exists(newPath) && !std::filesystem::exists(newProjectFile))
					{
						std::error_code ec;
						std::filesystem::copy_file(newPath, newProjectFile, std::filesystem::copy_options::none, ec);
					}

					if (std::filesystem::exists(newProjectFile))
					{
						std::ifstream in(newProjectFile);
						std::string content((std::istreambuf_iterator<char>(in)),
							std::istreambuf_iterator<char>());
						in.close();

						size_t pos = 0;
						while ((pos = content.find(oldStem, pos)) != std::string::npos)
						{
							content.replace(pos, oldStem.size(), newStem);
							pos += newStem.size();
						}

						std::ofstream out(newProjectFile);
						out << content;
					}
				}
			}

			// Scene files carry their displayed name inside the JSON. Without
			// this sync, renaming Foo.scene -> Bar.scene leaves the embedded
			// name as "Foo", and the hierarchy still shows "Foo" the next time
			// the file is opened.
			if (ext == ".scene")
			{
				SyncSceneEmbeddedNameToFilename(newPath, newStem);
				UpdateLoadedSceneNameAfterRename(oldStem, newStem);
			}

			m_NeedsRefresh = true;
		}
	}

	void AssetBrowser::SyncSceneEmbeddedNameIfNeeded(const std::filesystem::path& copiedPath) {
		if (copiedPath.extension() != ".scene") return;
		SyncSceneEmbeddedNameToFilename(copiedPath.string(), copiedPath.stem().string());
	}

	void AssetBrowser::CopyPathToClipboard(const std::string& path) {
		ImGui::SetClipboardText(path.c_str());
	}

	void AssetBrowser::CreateAssetWithCollisionCheck(
		std::filesystem::path preferredPath,
		std::function<void(const std::filesystem::path&)> doCreate)
	{
		std::error_code ec;
		const bool exists = std::filesystem::exists(preferredPath, ec);
		if (!exists || ec) {
			doCreate(preferredPath);
			return;
		}

		auto prompt = std::make_unique<PendingCreationPrompt>();
		prompt->PreferredPath = std::move(preferredPath);
		prompt->DisplayName = prompt->PreferredPath.filename().string();
		prompt->DoCreate = std::move(doCreate);
		m_PendingCreationPrompt = std::move(prompt);
		m_OpenCreationPromptThisFrame = true;
	}

	void AssetBrowser::RenderCreationCollisionPrompt() {
		constexpr const char* k_PromptId = "Asset already exists###AssetBrowserCollision";

		if (m_OpenCreationPromptThisFrame) {
			ImGui::OpenPopup(k_PromptId);
			m_OpenCreationPromptThisFrame = false;
		}

		if (!m_PendingCreationPrompt) return;

		// Center on the main viewport (= editor window). Promoted to a real OS
		// window via SetNextWindowAsNativeDialog so the prompt isn't trapped
		// inside the editor and still works while the editor is unfocused.
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();

		if (ImGui::BeginPopupModal(k_PromptId, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::TextWrapped("An asset named \"%s\" already exists in this folder.",
				m_PendingCreationPrompt->DisplayName.c_str());
			ImGui::Spacing();
			ImGui::TextDisabled("What would you like to do?");
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			bool decided = false;
			// Move the prompt out before invoking DoCreate so the callback can
			// re-enter CreateAssetWithCollisionCheck without trampling state.
			auto finishWith = [&](const std::filesystem::path& finalPath, bool deleteExisting) {
				auto local = std::move(m_PendingCreationPrompt);
				m_PendingCreationPrompt.reset();
				if (deleteExisting) {
					std::error_code rmEc;
					std::filesystem::remove(local->PreferredPath, rmEc);
					if (rmEc) {
						IDX_WARN_TAG("AssetBrowser",
							"Overwrite: failed to remove existing file '{}': {}",
							local->PreferredPath.string(), rmEc.message());
					}
				}
				local->DoCreate(finalPath);
				decided = true;
			};

			if (ImGui::Button("Overwrite", ImVec2(120, 0))) {
				finishWith(m_PendingCreationPrompt->PreferredPath, /*deleteExisting=*/true);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Replace the existing file. The previous one is permanently removed.");
			}

			ImGui::SameLine();
			if (ImGui::Button("Make Copy", ImVec2(120, 0))) {
				const std::filesystem::path parent = m_PendingCreationPrompt->PreferredPath.parent_path();
				const std::string stem = m_PendingCreationPrompt->PreferredPath.stem().string();
				const std::string ext = m_PendingCreationPrompt->PreferredPath.extension().string();
				std::filesystem::path candidate = m_PendingCreationPrompt->PreferredPath;
				std::error_code ec;
				for (int n = 1; std::filesystem::exists(candidate, ec) && n < 10000; ++n) {
					candidate = parent / (FormatDuplicateAssetName(stem, n) + ext);
					ec.clear();
				}
				finishWith(candidate, /*deleteExisting=*/false);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Keep the existing file; create the new one with a numbered suffix.");
			}

			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0))) {
				m_PendingCreationPrompt.reset();
				decided = true;
			}

			if (!decided && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				m_PendingCreationPrompt.reset();
				decided = true;
			}

			if (decided) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	void AssetBrowser::RequestDeleteSelectedAssets() {
		const std::vector<std::string> paths = GetSelectedPaths();
		if (paths.empty()) {
			return;
		}

		// Confirmation disabled in preferences: delete straight away.
		if (!EditorPreferences::GetConfirmOnDeleteAsset()) {
			DeleteSelectedAssets();
			return;
		}

		m_PendingDeletePaths = paths;
		m_OpenDeletePromptThisFrame = true;
	}

	void AssetBrowser::RenderDeleteConfirmationPrompt() {
		constexpr const char* k_DeleteId = "Delete Asset###AssetBrowserDelete";

		if (m_OpenDeletePromptThisFrame) {
			ImGui::OpenPopup(k_DeleteId);
			m_OpenDeletePromptThisFrame = false;
		}

		if (m_PendingDeletePaths.empty()) return;

		// Same native-dialog centering as RenderCreationCollisionPrompt so the
		// prompt isn't trapped inside the editor window.
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();

		if (ImGui::BeginPopupModal(k_DeleteId, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			if (m_PendingDeletePaths.size() == 1) {
				const std::string name = std::filesystem::path(m_PendingDeletePaths.front()).filename().string();
				ImGui::TextWrapped("Delete \"%s\"?", name.c_str());
			}
			else {
				ImGui::TextWrapped("Delete %zu selected items?", m_PendingDeletePaths.size());
			}
			ImGui::Spacing();
			ImGui::TextDisabled("This cannot be undone.");
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			bool decided = false;
			if (ImGui::Button("Delete", ImVec2(120, 0))) {
				StartAssetDeletion(m_PendingDeletePaths);
				decided = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				decided = true;
			}

			if (decided) {
				m_PendingDeletePaths.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	void AssetBrowser::CreateFolder(const std::string& parentDir) {
		std::string baseName = "New Folder";
		std::string folderPath = (std::filesystem::path(parentDir) / baseName).string();
		int counter = 1;
		while (Directory::Exists(folderPath)) {
			folderPath = (std::filesystem::path(parentDir) / (baseName + " " + std::to_string(counter))).string();
			counter++;
		}

		Directory::Create(folderPath, false);
		m_NeedsRefresh = true;

		Refresh();

		m_SelectedPath = folderPath;
		std::string name = std::filesystem::path(folderPath).filename().string();
		BeginRename(folderPath, name);
	}

	void AssetBrowser::CreateScript(const std::string& parentDir) {
		const std::filesystem::path preferred = std::filesystem::path(parentDir) / "NewEntityScript.cs";
		CreateAssetWithCollisionCheck(preferred,
			[this, parentDir](const std::filesystem::path& finalPath) {
				m_PendingScriptType = PendingScriptType::CSharp;
				m_PendingScriptDir = parentDir;
				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPath.string();
				BeginRename(finalPath.string(), finalPath.stem().string());
			});
	}

	void AssetBrowser::CreateManagedCSharpComponent(const std::string& parentDir) {
		const std::filesystem::path preferred = std::filesystem::path(parentDir) / "NewComponent.cs";
		CreateAssetWithCollisionCheck(preferred,
			[this, parentDir](const std::filesystem::path& finalPath) {
				m_PendingScriptType = PendingScriptType::CSharpComponent;
				m_PendingScriptDir = parentDir;
				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPath.string();
				BeginRename(finalPath.string(), finalPath.stem().string());
			});
	}

	void AssetBrowser::CreateNativeCSharpComponent(const std::string& parentDir) {
		const std::filesystem::path preferred = std::filesystem::path(parentDir) / "NewNativeComponent.cs";
		CreateAssetWithCollisionCheck(preferred,
			[this, parentDir](const std::filesystem::path& finalPath) {
				m_PendingScriptType = PendingScriptType::CSharpNativeComponent;
				m_PendingScriptDir = parentDir;
				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPath.string();
				BeginRename(finalPath.string(), finalPath.stem().string());
			});
	}

	void AssetBrowser::CreateSceneSystem(const std::string& parentDir) {
		const std::filesystem::path preferred = std::filesystem::path(parentDir) / "NewSceneSystem.cs";
		CreateAssetWithCollisionCheck(preferred,
			[this, parentDir](const std::filesystem::path& finalPath) {
				m_PendingScriptType = PendingScriptType::CSharpSceneSystem;
				m_PendingScriptDir = parentDir;
				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPath.string();
				BeginRename(finalPath.string(), finalPath.stem().string());
			});
	}

	void AssetBrowser::CreateGlobalSystem(const std::string& parentDir) {
		const std::filesystem::path preferred = std::filesystem::path(parentDir) / "NewGlobalSystem.cs";
		CreateAssetWithCollisionCheck(preferred,
			[this, parentDir](const std::filesystem::path& finalPath) {
				m_PendingScriptType = PendingScriptType::CSharpGlobalSystem;
				m_PendingScriptDir = parentDir;
				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPath.string();
				BeginRename(finalPath.string(), finalPath.stem().string());
			});
	}

	void AssetBrowser::CreateEmptyScript(const std::string& parentDir) {
		const std::filesystem::path preferred = std::filesystem::path(parentDir) / "NewScript.cs";
		CreateAssetWithCollisionCheck(preferred,
			[this, parentDir](const std::filesystem::path& finalPath) {
				m_PendingScriptType = PendingScriptType::CSharpEmpty;
				m_PendingScriptDir = parentDir;
				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPath.string();
				BeginRename(finalPath.string(), finalPath.stem().string());
			});
	}

	void AssetBrowser::CreateDataAssetScript(const std::string& parentDir) {
		const std::filesystem::path preferred = std::filesystem::path(parentDir) / "NewDataAsset.cs";
		CreateAssetWithCollisionCheck(preferred,
			[this, parentDir](const std::filesystem::path& finalPath) {
				m_PendingScriptType = PendingScriptType::CSharpDataAsset;
				m_PendingScriptDir = parentDir;
				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPath.string();
				BeginRename(finalPath.string(), finalPath.stem().string());
			});
	}

	
	void AssetBrowser::CreateDefaultTexture(const std::string& parentDir,
		const std::string& sourceFile, const std::string& displayName)
	{
		const std::string sourceDir = Path::ResolveIndexAssets("Textures/Default");
		if (sourceDir.empty()) {
			IDX_WARN_TAG("AssetBrowser",
				"Default textures directory not found (IndexAssets/Textures/Default). "
				"Cannot create '{}'.", displayName);
			return;
		}

		const std::filesystem::path source =
			std::filesystem::path(sourceDir) / sourceFile;
		if (!std::filesystem::exists(source)) {
			IDX_WARN_TAG("AssetBrowser",
				"Default texture '{}' not found at {}.",
				sourceFile, source.string());
			return;
		}

		const std::string ext = source.extension().string();
		const std::filesystem::path preferred =
			std::filesystem::path(parentDir) / (displayName + ext);

		CreateAssetWithCollisionCheck(preferred,
			[this, source](const std::filesystem::path& finalPath) {
				try {
					std::filesystem::copy_file(source, finalPath,
						std::filesystem::copy_options::overwrite_existing);
				}
				catch (const std::exception& e) {
					IDX_ERROR_TAG("AssetBrowser",
						"Failed to copy default texture '{}' to '{}': {}",
						source.string(), finalPath.string(), e.what());
					return;
				}

				const std::string finalPathStr = finalPath.string();
				AssetRegistry::ImportAssetPathIncremental(finalPathStr);
				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPathStr;
				BeginRename(finalPathStr, finalPath.stem().string());
			});
	}

	void AssetBrowser::CreateScene(const std::string& parentDir) {
		const std::filesystem::path preferred =
			std::filesystem::path(parentDir) / "NewScene.scene";

		CreateAssetWithCollisionCheck(preferred,
			[this](const std::filesystem::path& finalPath) {
				const std::string sceneName = finalPath.stem().string();
				const std::string content =
					"{\n"
					"  \"name\": \"" + sceneName + "\",\n"
					"  \"systems\": [],\n"
					"  \"entities\": [\n"
					"    {\n"
					"      \"name\": \"Camera\",\n"
					"      \"Transform2D\": { \"posX\": 0, \"posY\": 0, \"rotation\": 0, \"scaleX\": 1, \"scaleY\": 1 },\n"
					"      \"Camera2D\": { \"orthoSize\": 5, \"zoom\": 1 }\n"
					"    }\n"
					"  ]\n"
					"}\n";

				const std::string finalPathStr = finalPath.string();
				std::ofstream file(finalPathStr);
				if (file.is_open()) {
					file << content;
					file.close();
				}

				AssetRegistry::ImportAssetPathIncremental(finalPathStr);
				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPathStr;
				BeginRename(finalPathStr, finalPath.filename().string());
			});
	}

	void AssetBrowser::CreateDataAsset(const std::string& parentDir, const std::string& typeName) {
		const std::filesystem::path preferred =
			std::filesystem::path(parentDir) / (typeName + ".dataasset");

		CreateAssetWithCollisionCheck(preferred,
			[this, typeName](const std::filesystem::path& finalPath) {
				const std::string finalPathStr = finalPath.string();
				// DataAssetManager::Create writes the {version,type,fields} stub and registers the GUID.
				if (DataAssetManager::Create(finalPathStr, typeName) == 0)
					return;

				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPathStr;
				BeginRename(finalPathStr, finalPath.stem().string());
			});
	}

	void AssetBrowser::CreateFile(const std::string& parentDir, const std::string& baseName,
		const std::string& extension, const std::string& defaultContent) {
		// Normalize: extension may or may not include the leading dot.
		const std::string ext = (extension.empty() || extension[0] == '.') ? extension : ("." + extension);

		const std::filesystem::path preferred =
			std::filesystem::path(parentDir) / (baseName + ext);

		CreateAssetWithCollisionCheck(preferred,
			[this, defaultContent](const std::filesystem::path& finalPath) {
				const std::string finalPathStr = finalPath.string();
				std::ofstream file(finalPathStr, std::ios::binary);
				if (file.is_open()) {
					if (!defaultContent.empty()) {
						file.write(defaultContent.data(),
							static_cast<std::streamsize>(defaultContent.size()));
					}
					file.close();
				}

				AssetRegistry::ImportAssetPathIncremental(finalPathStr);
				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPathStr;
				BeginRename(finalPathStr,
					std::filesystem::path(finalPathStr).stem().string());
			});
	}

	void AssetBrowser::CreateEntityPrefab(const std::string& parentDir, EntityHandle sourceEntity) {
		const std::filesystem::path preferred =
			std::filesystem::path(parentDir) / "NewPrefab.prefab";

		CreateAssetWithCollisionCheck(preferred,
			[this, parentDir, sourceEntity](const std::filesystem::path& finalPath) {
				const std::string finalPathStr = finalPath.string();

				if (sourceEntity != entt::null) {
					Scene* activeScene = SceneManager::Get().GetActiveScene();
					if (activeScene && activeScene->IsValid(sourceEntity)) {
						SceneSerializer::SaveEntityToFile(*activeScene, sourceEntity, finalPathStr);
					}
				}
				else {
					const std::string content =
						"{\n"
						"  \"version\": 1,\n"
						"  \"type\": \"Prefab\",\n"
						"  \"Entity\": {\n"
						"    \"Transform2D\": { \"posX\": 0, \"posY\": 0, \"rotation\": 0, \"scaleX\": 1, \"scaleY\": 1 }\n"
						"  },\n"
						"  \"prefab\": {\n"
						"    \"Transform2D\": { \"posX\": 0, \"posY\": 0, \"rotation\": 0, \"scaleX\": 1, \"scaleY\": 1 }\n"
						"  }\n"
						"}\n";
					std::ofstream file(finalPathStr);
					if (file.is_open()) {
						file << content;
						file.close();
					}
				}

				AssetRegistry::ImportAssetPathIncremental(finalPathStr);
				m_PendingScriptType = PendingScriptType::EntityPrefab;
				m_PendingScriptDir = parentDir;
				m_PendingPrefabSourceEntity = sourceEntity;
				m_NeedsRefresh = true;
				Refresh();
				m_SelectedPath = finalPathStr;
				BeginRename(finalPathStr, finalPath.filename().string());
			});
	}

	void AssetBrowser::OpenAssetExternal(const DirectoryEntry& entry) {
		try
		{
			std::string ext = std::filesystem::path(entry.Path).extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

			if (ext == ".scene")
			{
				m_PendingSceneLoad = entry.Path;
				return;
			}

			if (ext == ".prefab")
			{
				// Defer: ImGuiEditorLayer drains this in OnPreRender to enter prefab-edit mode; routing here keeps prefab and scene double-click behaviour in the same place.
				m_PendingPrefabEdit = entry.Path;
				return;
			}

			if (ExternalEditor::IsScriptExtension(ext))
			{
				std::filesystem::path scriptPath = entry.Path;
				const std::filesystem::path nativeMirrorPath = ResolveNativeScriptMirrorPath(entry.Path);
				if (!nativeMirrorPath.empty() && std::filesystem::exists(nativeMirrorPath)) {
					scriptPath = nativeMirrorPath;
				}
				ExternalEditor::OpenFile(scriptPath.string());
				return;
			}

#ifdef IDX_PLATFORM_WINDOWS
			// ShellExecuteW is needed for OS file-association handling; threads are tracked (not detached) so Shutdown can join them.
			static constexpr int k_MaxConcurrentOpens = 8;
			static std::atomic<int> s_ActiveOpens{ 0 };
			if (s_ActiveOpens.load(std::memory_order_acquire) >= k_MaxConcurrentOpens) {
				IDX_WARN_TAG("AssetBrowser",
					"Skipping external open for '{}' — too many concurrent shell launches.",
					entry.Path);
				return;
			}
			s_ActiveOpens.fetch_add(1, std::memory_order_acq_rel);

			std::wstring wpath = std::filesystem::absolute(entry.Path).wstring();
			std::thread worker([wpath]() {
				CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				CoUninitialize();
				s_ActiveOpens.fetch_sub(1, std::memory_order_acq_rel);
			});
			TrackShellLaunchThread(std::move(worker));
#endif
		}
		catch (...) {}
	}

	void AssetBrowser::StartExternalAssetImport(std::vector<std::string> paths, std::string destinationDirectory) {
		if (paths.empty() || destinationDirectory.empty()) {
			return;
		}

		// Import and delete share the single Win32 progress popup and the
		// (unsynchronized) AssetRegistry, so they must not overlap. A delete already in
		// flight wins; the drop is ignored (same behaviour as dropping mid-import).
		if (m_DeleteActive.load(std::memory_order_acquire)) {
			IDX_CORE_WARN_TAG("AssetBrowser", "Asset deletion is in progress; ignoring {} dropped path(s).", paths.size());
			return;
		}

		if (m_ImportActive.exchange(true, std::memory_order_acq_rel)) {
			IDX_CORE_WARN_TAG("AssetBrowser", "Asset import is already running; ignoring {} dropped path(s).", paths.size());
			return;
		}

		if (m_ImportThread.joinable()) {
			m_ImportThread.join();
		}

		{
			std::scoped_lock lock(m_ImportMutex);
			m_ImportWorkerFinished = false;
			m_ImportResultsConsumed = true;
			m_ImportDestinationDirectory = destinationDirectory;
			m_ImportCurrentItem = "Preparing...";
			m_ImportTotalBytes = 0;
			m_ImportCopiedBytes = 0;
			m_ImportTotalRootItems = paths.size();
			m_ImportProcessedRootItems = 0;
			m_ImportImportedRootCount = 0;
			m_ImportImportedRootPaths.clear();
			m_ImportImportedAssetPaths.clear();
			m_ImportErrors.clear();
		}

		try {
			m_ImportThread = std::thread(
				&AssetBrowser::RunExternalAssetImport,
				this,
				std::move(paths),
				std::move(destinationDirectory));
		}
		catch (const std::exception& e) {
			m_ImportActive.store(false, std::memory_order_release);
			IDX_CORE_ERROR_TAG("AssetBrowser", "Failed to start asset import worker: {}", e.what());
		}
	}

	void AssetBrowser::RunExternalAssetImport(std::vector<std::string> paths, std::string destinationDirectory) {
		namespace fs = std::filesystem;

		// Boundary guard: a throw escaping this std::thread body calls std::terminate and kills the
		// editor. Most fs calls here use error_code, but allocation (the copy buffer, path strings) can
		// still throw — catch and publish a finished+error state so the import UI recovers instead.
		try {

		std::vector<std::string> importedRootPaths;
		std::vector<std::string> importedAssetPaths;
		std::vector<std::string> errors;
		std::size_t importedRootCount = 0;

		auto publishCurrentItem = [this](const std::string& item) {
			std::scoped_lock lock(m_ImportMutex);
			m_ImportCurrentItem = item;
		};

		auto publishCopiedBytes = [this](std::uintmax_t bytes) {
			std::scoped_lock lock(m_ImportMutex);
			m_ImportCopiedBytes += bytes;
		};

		auto publishProcessedRoot = [this](std::size_t processed) {
			std::scoped_lock lock(m_ImportMutex);
			m_ImportProcessedRootItems = processed;
		};

		auto appendError = [&errors](const fs::path& path, const std::string& message) {
			errors.push_back("Failed to import '" + path.string() + "': " + message);
		};

		publishCurrentItem("Scanning...");
		std::uintmax_t totalBytes = 0;
		for (const std::string& pathString : paths) {
			if (!m_ImportActive.load(std::memory_order_acquire)) {
				break;
			}

			std::error_code ec;
			const fs::path path(pathString);
			if (fs::exists(path, ec) && !ec) {
				CollectImportByteSize(path, totalBytes);
			}
		}

		{
			std::scoped_lock lock(m_ImportMutex);
			m_ImportTotalBytes = totalBytes;
		}

		const fs::path destDir(destinationDirectory);
		for (std::size_t i = 0; i < paths.size(); ++i) {
			if (!m_ImportActive.load(std::memory_order_acquire)) {
				break;
			}

			const fs::path src(paths[i]);
			publishCurrentItem(src.filename().empty() ? src.string() : src.filename().string());

			std::error_code ec;
			if (!fs::exists(src, ec) || ec) {
				appendError(src, ec ? ec.message() : "source path does not exist");
				publishProcessedRoot(i + 1);
				continue;
			}

			bool importedThisRoot = false;
			fs::path importedRootPath;

			if (fs::is_directory(src, ec) && !ec) {
				const fs::path destRoot = destDir / src.filename();
				fs::create_directories(destRoot, ec);
				if (ec) {
					appendError(src, ec.message());
					publishProcessedRoot(i + 1);
					continue;
				}

				importedThisRoot = true;
				importedRootPath = destRoot;

				for (fs::recursive_directory_iterator it(
					 src,
					 fs::directory_options::skip_permission_denied,
					 ec), end;
					 it != end && m_ImportActive.load(std::memory_order_acquire);
					 it.increment(ec)) {
					if (ec) {
						ec.clear();
						continue;
					}

					std::error_code relEc;
					const fs::path rel = fs::relative(it->path(), src, relEc);
					if (relEc) {
						continue;
					}

					const fs::path dest = destRoot / rel;
					std::error_code entryEc;
					if (it->is_directory(entryEc) && !entryEc) {
						fs::create_directories(dest, entryEc);
						if (entryEc) {
							appendError(it->path(), entryEc.message());
						}
						continue;
					}

					entryEc.clear();
					if (!it->is_regular_file(entryEc) || entryEc) {
						continue;
					}

					if (fs::exists(dest, entryEc) && !entryEc) {
						publishCopiedBytes(SafeFileSize(it->path()));
						continue;
					}

					if (CopyFileChunked(it->path(), dest, false, m_ImportActive, publishCopiedBytes)) {
						AddImportedAssetPath(importedAssetPaths, dest);
					}
					else if (m_ImportActive.load(std::memory_order_acquire)) {
						appendError(it->path(), "copy failed");
					}
				}
			}
			else if (fs::is_regular_file(src, ec) && !ec) {
				const fs::path dest = MakeUniqueImportFilePath(src, destDir);
				if (CopyFileChunked(src, dest, false, m_ImportActive, publishCopiedBytes)) {
					importedThisRoot = true;
					importedRootPath = dest;
					AddImportedAssetPath(importedAssetPaths, dest);
				}
				else if (m_ImportActive.load(std::memory_order_acquire)) {
					appendError(src, "copy failed");
				}
			}

			if (importedThisRoot) {
				++importedRootCount;
				importedRootPaths.push_back(importedRootPath.string());
			}

			publishProcessedRoot(i + 1);
		}

		{
			std::scoped_lock lock(m_ImportMutex);
			m_ImportCurrentItem = m_ImportActive.load(std::memory_order_acquire) ? "Copy complete" : "Cancelled";
			m_ImportImportedRootCount = importedRootCount;
			m_ImportImportedRootPaths = std::move(importedRootPaths);
			m_ImportImportedAssetPaths = std::move(importedAssetPaths);
			m_ImportErrors = std::move(errors);
			m_ImportWorkerFinished = true;
			m_ImportResultsConsumed = false;
		}
		}
		catch (const std::exception& e) {
			IDX_CORE_ERROR_TAG("AssetBrowser", "Asset import worker crashed: {}", e.what());
			std::scoped_lock lock(m_ImportMutex);
			m_ImportCurrentItem = "Import failed";
			m_ImportErrors.push_back(std::string("Import failed: ") + e.what());
			m_ImportWorkerFinished = true;
			m_ImportResultsConsumed = false;
		}
		catch (...) {
			IDX_CORE_ERROR_TAG("AssetBrowser", "Asset import worker crashed: unknown exception");
			std::scoped_lock lock(m_ImportMutex);
			m_ImportCurrentItem = "Import failed";
			m_ImportErrors.push_back("Import failed: unknown error");
			m_ImportWorkerFinished = true;
			m_ImportResultsConsumed = false;
		}
	}

	void AssetBrowser::ConsumeFinishedExternalAssetImport() {
		bool shouldConsume = false;
		{
			std::scoped_lock lock(m_ImportMutex);
			shouldConsume = m_ImportWorkerFinished && !m_ImportResultsConsumed;
		}

		if (!shouldConsume) {
			return;
		}

		if (m_ImportThread.joinable()) {
			m_ImportThread.join();
		}

		std::vector<std::string> errors;
		{
			std::scoped_lock lock(m_ImportMutex);
			m_PendingImportedAssetRegistrations = std::move(m_ImportImportedAssetPaths);
			m_PendingImportRootPaths = std::move(m_ImportImportedRootPaths);
			m_PendingImportRootCount = m_ImportImportedRootCount;
			m_PendingImportDestinationDirectory = m_ImportDestinationDirectory;
			m_PendingImportedAssetRegistrationIndex = 0;
			errors = std::move(m_ImportErrors);
			m_ImportResultsConsumed = true;
		}

		for (const std::string& error : errors) {
			IDX_CORE_WARN_TAG("AssetBrowser", "{}", error);
		}
	}

	void AssetBrowser::ProcessPendingImportedAssetRegistrations() {
		if (!m_ImportActive.load(std::memory_order_acquire)) {
			return;
		}

		const bool workerDone = [&]() {
			std::scoped_lock lock(m_ImportMutex);
			return m_ImportWorkerFinished && m_ImportResultsConsumed;
		}();

		if (!workerDone) {
			return;
		}

		const auto start = std::chrono::steady_clock::now();
		constexpr auto kBudget = std::chrono::milliseconds(4);
		std::size_t processedThisFrame = 0;

		while (m_PendingImportedAssetRegistrationIndex < m_PendingImportedAssetRegistrations.size()) {
			const std::string& path = m_PendingImportedAssetRegistrations[m_PendingImportedAssetRegistrationIndex];
			AssetRegistry::ImportAssetPathIncremental(path);
			m_Thumbnails.Invalidate(path);
			++m_PendingImportedAssetRegistrationIndex;
			++processedThisFrame;

			if (processedThisFrame >= 1 && std::chrono::steady_clock::now() - start >= kBudget) {
				return;
			}
		}

		if (m_PendingImportRootCount > 0) {
			if (m_CurrentDirectory == m_PendingImportDestinationDirectory) {
				m_SelectedPaths = m_PendingImportRootPaths;
				m_SelectedPath = m_PendingImportRootPaths.empty() ? std::string{} : m_PendingImportRootPaths.back();
				m_LastSelectionIndex = -1;
			}
			m_SkipRegistrySyncOnNextRefresh = true;
			m_NeedsRefresh = true;
			m_SuppressNextAssetWatcherRefresh.store(true, std::memory_order_release);
			IDX_CORE_INFO_TAG("AssetBrowser",
				"Imported {} item(s) into {}",
				m_PendingImportRootCount,
				m_PendingImportDestinationDirectory);
		}

		m_PendingImportedAssetRegistrations.clear();
		m_PendingImportRootPaths.clear();
		m_PendingImportedAssetRegistrationIndex = 0;
		m_PendingImportRootCount = 0;
		m_PendingImportDestinationDirectory.clear();
		m_ImportActive.store(false, std::memory_order_release);
	}

	void AssetBrowser::UpdateExternalAssetImportProgressWindow() {
		if (!m_ImportActive.load(std::memory_order_acquire)) {
			if (m_ImportProgressWindowVisible) {
				Win32BuildProgressWindow::Hide();
				m_ImportProgressWindowVisible = false;
			}
			return;
		}

		if (!m_ImportProgressWindowVisible) {
			Win32BuildProgressWindow::Show("Importing Assets...");
			m_ImportProgressWindowVisible = true;
		}

		float progress = 0.0f;
		std::string stage = "Preparing...";

		const bool workerDone = [&]() {
			std::scoped_lock lock(m_ImportMutex);
			return m_ImportWorkerFinished && m_ImportResultsConsumed;
		}();

		if (workerDone) {
			const std::size_t total = m_PendingImportedAssetRegistrations.size();
			if (total > 0) {
				progress = 0.90f + 0.10f * (
					static_cast<float>(m_PendingImportedAssetRegistrationIndex) /
					static_cast<float>(total));
				stage = "Indexing assets...";
			}
			else {
				progress = 1.0f;
				stage = "Done";
			}
		}
		else {
			std::uintmax_t totalBytes = 0;
			std::uintmax_t copiedBytes = 0;
			std::size_t totalRoots = 0;
			std::size_t processedRoots = 0;
			{
				std::scoped_lock lock(m_ImportMutex);
				totalBytes = m_ImportTotalBytes;
				copiedBytes = m_ImportCopiedBytes;
				totalRoots = m_ImportTotalRootItems;
				processedRoots = m_ImportProcessedRootItems;
				stage = m_ImportCurrentItem.empty() ? "Copying..." : m_ImportCurrentItem;
			}

			if (totalBytes > 0) {
				progress = 0.90f * std::min(
					1.0f,
					static_cast<float>(copiedBytes) / static_cast<float>(totalBytes));
			}
			else if (totalRoots > 0) {
				progress = 0.10f * (
					static_cast<float>(processedRoots) / static_cast<float>(totalRoots));
			}
		}

		Win32BuildProgressWindow::Update(progress, stage);
	}

	void AssetBrowser::PumpExternalAssetImport() {
		ConsumeFinishedExternalAssetImport();
		ProcessPendingImportedAssetRegistrations();
		UpdateExternalAssetImportProgressWindow();
	}

	void AssetBrowser::OnExternalFileDrop(const std::vector<std::string>& paths) {
		if (m_CurrentDirectory.empty()) return;
		if (paths.empty()) return;

		StartExternalAssetImport(paths, m_CurrentDirectory);
	}

	void AssetBrowser::StartAssetDeletion(std::vector<std::string> paths) {
		if (paths.empty()) {
			return;
		}

		// The Win32 progress popup and the AssetRegistry are single-owner, shared
		// resources. If an import or a prior delete is still in flight, fall back to a
		// synchronous delete (only this rare overlap can briefly block) rather than
		// racing the other operation for the window / registry maps.
		if (m_ImportActive.load(std::memory_order_acquire) || m_DeleteActive.load(std::memory_order_acquire)) {
			for (const std::string& path : paths) {
				DeleteEntry(path);
			}
			return;
		}

		// A finished-but-not-yet-reaped worker can linger if the editor closed the
		// progress flow early; drain it before reusing the thread handle.
		if (m_DeleteThread.joinable()) {
			m_DeleteThread.join();
		}

		// Main-thread, pre-spawn bookkeeping. The actual filesystem removal is the slow
		// part and runs on the worker; everything that needs the files to still exist
		// (native-mirror resolution) or touches render/UI state happens here, and the
		// registry/scene/selection cleanup is deferred to the pump once the worker
		// reports which roots actually got deleted.
		std::vector<DeleteRootInfo> roots;
		roots.reserve(paths.size());
		for (const std::string& path : paths) {
			// Defer thumbnail invalidation to next frame's Render() exactly as the sync
			// DeleteEntry did — destroying the GPU texture now would dangle a
			// WGPUTextureView mid-frame (see m_PendingThumbnailInvalidates docs).
			m_PendingThumbnailInvalidates.push_back(path);

			DeleteRootInfo info;
			info.Path = path;
			std::error_code ec;
			info.IsDirectory = std::filesystem::is_directory(path, ec) && !ec;

			const std::filesystem::path mirror = ResolveNativeScriptMirrorPath(path);
			info.NativeMirrorPath = mirror.empty() ? std::string{} : mirror.string();

			const std::filesystem::path pathFs(path);
			if (!info.IsDirectory && pathFs.extension() == ".scene") {
				info.SceneStem = pathFs.stem().string();
			}

			roots.push_back(std::move(info));
		}

		// A pending rename targeting one of these paths would otherwise resolve against
		// a vanished file; cancel it now (matches the old sync DeleteEntry).
		CancelRename();

		{
			std::scoped_lock lock(m_DeleteMutex);
			m_DeleteWorkerFinished = false;
			m_DeleteResultsConsumed = true;
			m_DeleteCurrentItem = "Preparing...";
			m_DeleteForgetAssetPaths.clear();
			m_DeleteSucceededRoots.clear();
			m_DeleteErrors.clear();
		}
		// m_DeleteRoots / m_DeleteStartTime need no atomics or lock: the std::thread
		// constructor below establishes a happens-before edge, so every write here is
		// visible to the worker, and the worker is join()ed before they are touched
		// again (pump / next Start). m_DeleteStartTime is then only ever read on the
		// main thread.
		m_DeleteRoots = std::move(roots);
		m_DeleteTotalEntries.store(0, std::memory_order_release);
		m_DeleteProcessedEntries.store(0, std::memory_order_release);
		m_DeleteStartTime = std::chrono::steady_clock::now();
		m_DeleteActive.store(true, std::memory_order_release);

		try {
			m_DeleteThread = std::thread(&AssetBrowser::RunAssetDeletion, this);
		}
		catch (const std::exception& e) {
			m_DeleteActive.store(false, std::memory_order_release);
			IDX_CORE_ERROR_TAG("AssetBrowser", "Failed to start asset deletion worker: {}", e.what());
			// Worker never launched — complete the user's action synchronously.
			std::vector<DeleteRootInfo> fallback = std::move(m_DeleteRoots);
			m_DeleteRoots.clear();
			for (const DeleteRootInfo& info : fallback) {
				DeleteEntry(info.Path);
			}
		}
	}

	void AssetBrowser::RunAssetDeletion() {
		namespace fs = std::filesystem;

		// Boundary guard: a throw escaping this std::thread body calls std::terminate
		// and kills the editor. fs calls use error_code, but allocation (the path
		// vectors) can still throw — catch and publish a finished+error state so the
		// deletion UI recovers instead.
		try {

		auto publishCurrentItem = [this](const std::string& item) {
			std::scoped_lock lock(m_DeleteMutex);
			m_DeleteCurrentItem = item;
		};

		// --- Pass 1: enumerate every entry up front so the progress bar is determinate. ---
		struct RootPlan {
			const DeleteRootInfo* Info = nullptr;
			std::vector<fs::path> Removal;        // ordered children-before-parents
			std::vector<std::string> AssetPaths;  // non-.meta assets to forget on success
		};

		publishCurrentItem("Scanning...");
		std::vector<RootPlan> plans;
		plans.reserve(m_DeleteRoots.size());
		std::size_t totalEntries = 0;

		for (const DeleteRootInfo& info : m_DeleteRoots) {
			if (!m_DeleteActive.load(std::memory_order_acquire)) {
				break;
			}

			RootPlan plan;
			plan.Info = &info;

			std::error_code ec;
			if (info.IsDirectory) {
				std::vector<fs::path> descendants;
				for (fs::recursive_directory_iterator it(
					 info.Path, fs::directory_options::skip_permission_denied, ec), end;
					 it != end; it.increment(ec)) {
					// Bail a huge scan on shutdown (Shutdown sets m_DeleteActive=false).
					if (!m_DeleteActive.load(std::memory_order_acquire)) {
						break;
					}
					if (ec) { ec.clear(); continue; }

					// Never descend INTO a symlink/junction directory: it can point back
					// up the tree and spin the iterator into unbounded growth. The link
					// entry itself is still recorded below so the link is removed.
					std::error_code symEc;
					if (it->is_symlink(symEc)) {
						it.disable_recursion_pending();
					}

					descendants.push_back(it->path());

					std::error_code fileEc;
					if (it->is_regular_file(fileEc) && !fileEc
						&& !AssetRegistry::IsMetaFilePath(it->path().string())) {
						plan.AssetPaths.push_back(it->path().string());
					}
				}
				// recursive_directory_iterator is pre-order (parent before children);
				// reversing it removes children before their parent. The root directory
				// itself is appended last so it is empty by the time we remove it.
				plan.Removal.assign(descendants.rbegin(), descendants.rend());
				plan.Removal.emplace_back(info.Path);
			}
			else {
				plan.Removal.emplace_back(info.Path);
				if (!AssetRegistry::IsMetaFilePath(info.Path)) {
					plan.AssetPaths.push_back(info.Path);
				}
			}

			totalEntries += plan.Removal.size();
			plans.push_back(std::move(plan));
		}

		m_DeleteTotalEntries.store(totalEntries, std::memory_order_release);

		// --- Pass 2: remove entries bottom-up, publishing progress per entry. ---
		std::vector<std::string> forgetAssetPaths;
		std::vector<std::string> succeededRoots;
		std::vector<std::string> errors;

		for (const RootPlan& plan : plans) {
			if (!m_DeleteActive.load(std::memory_order_acquire)) {
				break;
			}

			const fs::path rootPath(plan.Info->Path);
			publishCurrentItem(rootPath.filename().empty()
				? plan.Info->Path : rootPath.filename().string());

			for (const fs::path& entry : plan.Removal) {
				if (!m_DeleteActive.load(std::memory_order_acquire)) {
					break;
				}
				std::error_code ec;
				fs::remove(entry, ec);
				m_DeleteProcessedEntries.fetch_add(1, std::memory_order_acq_rel);
			}

			if (!plan.Info->NativeMirrorPath.empty()) {
				std::error_code ec;
				fs::remove(plan.Info->NativeMirrorPath, ec);
			}

			// Treat the root as deleted only if it is provably gone: fs::exists returns
			// false on error too, so without the !existsEc guard a stat failure on a
			// still-present folder would wrongly forget its assets from the registry.
			std::error_code existsEc;
			const bool stillExists = fs::exists(plan.Info->Path, existsEc);
			if (!stillExists && !existsEc) {
				succeededRoots.push_back(plan.Info->Path);
				forgetAssetPaths.insert(forgetAssetPaths.end(),
					plan.AssetPaths.begin(), plan.AssetPaths.end());
			}
			else {
				errors.push_back("Failed to fully delete '" + plan.Info->Path + "'");
			}
		}

		{
			std::scoped_lock lock(m_DeleteMutex);
			m_DeleteCurrentItem = m_DeleteActive.load(std::memory_order_acquire) ? "Delete complete" : "Cancelled";
			m_DeleteForgetAssetPaths = std::move(forgetAssetPaths);
			m_DeleteSucceededRoots = std::move(succeededRoots);
			m_DeleteErrors = std::move(errors);
			m_DeleteWorkerFinished = true;
			m_DeleteResultsConsumed = false;
		}

		}
		catch (const std::exception& e) {
			IDX_CORE_ERROR_TAG("AssetBrowser", "Asset deletion worker crashed: {}", e.what());
			std::scoped_lock lock(m_DeleteMutex);
			m_DeleteCurrentItem = "Delete failed";
			m_DeleteErrors.push_back(std::string("Delete failed: ") + e.what());
			m_DeleteWorkerFinished = true;
			m_DeleteResultsConsumed = false;
		}
		catch (...) {
			IDX_CORE_ERROR_TAG("AssetBrowser", "Asset deletion worker crashed: unknown exception");
			std::scoped_lock lock(m_DeleteMutex);
			m_DeleteCurrentItem = "Delete failed";
			m_DeleteErrors.push_back("Delete failed: unknown error");
			m_DeleteWorkerFinished = true;
			m_DeleteResultsConsumed = false;
		}
	}

	void AssetBrowser::ConsumeAndApplyFinishedAssetDeletion() {
		bool shouldApply = false;
		{
			std::scoped_lock lock(m_DeleteMutex);
			shouldApply = m_DeleteWorkerFinished && !m_DeleteResultsConsumed;
		}
		if (!shouldApply) {
			return;
		}

		if (m_DeleteThread.joinable()) {
			m_DeleteThread.join();
		}

		std::vector<std::string> forgetAssetPaths;
		std::vector<std::string> succeededRoots;
		std::vector<std::string> errors;
		{
			std::scoped_lock lock(m_DeleteMutex);
			forgetAssetPaths = std::move(m_DeleteForgetAssetPaths);
			succeededRoots = std::move(m_DeleteSucceededRoots);
			errors = std::move(m_DeleteErrors);
			m_DeleteResultsConsumed = true;
		}

		const std::unordered_set<std::string> succeeded(succeededRoots.begin(), succeededRoots.end());

		// All of this is main-thread-only: AssetRegistry and SceneManager keep
		// unsynchronized global state, and selection fields are read by Render().
		for (const DeleteRootInfo& info : m_DeleteRoots) {
			if (succeeded.find(info.Path) == succeeded.end()) {
				continue;
			}

			// A single-file root's companion .meta sidecar is a sibling (not under the
			// deleted path), so the worker never removed it — do it here, which also
			// forgets the asset from the registry.
			if (!info.IsDirectory) {
				AssetRegistry::DeleteCompanionMetadata(info.Path);
			}

			// Drop the SceneDefinition so a script-side LoadScene of the deleted name
			// can't resurrect an empty Scene from the stale definition.
			if (!info.SceneStem.empty()) {
				SceneManager::Get().UnregisterScene(info.SceneStem);
			}

			if (m_SelectedPath == info.Path) {
				m_SelectedPath.clear();
			}
			m_SelectedPaths.erase(
				std::remove(m_SelectedPaths.begin(), m_SelectedPaths.end(), info.Path),
				m_SelectedPaths.end());
			if (m_PressedPath == info.Path) {
				m_PressedPath.clear();
			}
		}

		ForgetRegistryAssetPaths(forgetAssetPaths);

		for (const std::string& error : errors) {
			IDX_CORE_WARN_TAG("AssetBrowser", "{}", error);
		}

		m_DeleteRoots.clear();
		m_NeedsRefresh = true;
		// Mirror the import path: suppress the single asset-watcher refresh that would
		// otherwise fire right after we mutate the tree. Import and delete never overlap
		// (StartAssetDeletion serializes them), so reusing the import flag is safe.
		m_SuppressNextAssetWatcherRefresh.store(true, std::memory_order_release);
		m_DeleteActive.store(false, std::memory_order_release);
	}

	void AssetBrowser::UpdateAssetDeletionProgressWindow() {
		if (!m_DeleteActive.load(std::memory_order_acquire)) {
			if (m_DeleteProgressWindowVisible) {
				Win32BuildProgressWindow::Hide();
				m_DeleteProgressWindowVisible = false;
			}
			return;
		}

		if (!m_DeleteProgressWindowVisible) {
			// Grace period: a fast delete (a single small file) finishes before this
			// elapses, so the popup never flashes. Only genuinely slow deletes — big
			// folders, large or Defender-throttled removals — surface the window.
			constexpr auto kGrace = std::chrono::milliseconds(200);
			if (std::chrono::steady_clock::now() - m_DeleteStartTime < kGrace) {
				return;
			}
			Win32BuildProgressWindow::Show("Deleting Assets...");
			m_DeleteProgressWindowVisible = true;
		}

		const std::size_t total = m_DeleteTotalEntries.load(std::memory_order_acquire);
		const std::size_t processed = m_DeleteProcessedEntries.load(std::memory_order_acquire);
		const float progress = total > 0
			? std::min(1.0f, static_cast<float>(processed) / static_cast<float>(total))
			: 0.0f;

		std::string stage;
		{
			std::scoped_lock lock(m_DeleteMutex);
			stage = m_DeleteCurrentItem.empty() ? "Deleting..." : m_DeleteCurrentItem;
		}

		Win32BuildProgressWindow::Update(progress, stage);
	}

	void AssetBrowser::PumpAssetDeletion() {
		ConsumeAndApplyFinishedAssetDeletion();
		UpdateAssetDeletionProgressWindow();
	}

}
