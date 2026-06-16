#include "pch.hpp"
#include "Scripting/ScriptSystem.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/NativeScript.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Components/Tags.hpp"
#include "Physics/Collision2D.hpp"
#include "Core/Log.hpp"
#include "Core/Application.hpp"
#include "Profiling/Profiler.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/PrefabTemplateCache.hpp"
#include "Serialization/Json.hpp"
#include "Project/ProjectManager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <exception>
#include <filesystem>
#include <optional>
#include <regex>
#include <sstream>
#include <string>

#if defined(IDX_PLATFORM_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Index {
	struct ScriptSystemProcessTaskState {
		std::mutex Mutex;
		std::optional<Process::Result> Result;
		std::atomic<bool> Completed{ false };
		std::atomic<bool> Abandoned{ false };
		std::chrono::steady_clock::time_point StartedAt{ std::chrono::steady_clock::now() };

		std::thread Worker;

		~ScriptSystemProcessTaskState() {
			if (!Worker.joinable()) {
				return;
			}
			if (Completed.load(std::memory_order_acquire)) {
				// Worker function has already returned; just reap the thread.
				Worker.join();
			} else {
				Abandoned.store(true, std::memory_order_release);
				Worker.detach();
			}
		}
	};

	namespace {
		using ProcessTaskState = ScriptSystemProcessTaskState;

		std::string GetNativeLibraryFilename(const std::string& targetName)
		{
#if defined(IDX_PLATFORM_WINDOWS)
			return targetName + ".dll";
#elif defined(IDX_PLATFORM_LINUX)
			return "lib" + targetName + ".so";
#else
#error Unsupported platform for native scripts
#endif
		}

		std::filesystem::path NormalizeNativePath(std::filesystem::path path)
		{
			std::error_code ec;
			if (std::filesystem::exists(path)) {
				const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, ec);
				if (!ec) {
					return canonicalPath;
				}
			}

			return path.lexically_normal();
		}

		std::filesystem::path ResolveProjectNativeDLLPath(const IndexProject& project)
		{
			return NormalizeNativePath(std::filesystem::path(project.GetNativeDllPath()));
		}

		std::filesystem::path ResolveStandaloneNativeDLLPath(const std::filesystem::path& executableDirectory)
		{
			return NormalizeNativePath(
				executableDirectory / ".." / "Index-NativeScripts" / GetNativeLibraryFilename("Index-NativeScripts"));
		}

		std::string GetActiveNativeBuildConfig()
		{
			return IndexProject::GetActiveBuildConfiguration();
		}

		std::string TrimWhitespace(std::string value)
		{
			const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
				return std::isspace(ch) != 0;
			});
			const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
				return std::isspace(ch) != 0;
			}).base();

			if (first >= last) {
				return {};
			}

			return std::string(first, last);
		}

		std::string ToLowerCopy(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			return value;
		}

#if defined(IDX_PLATFORM_WINDOWS)
		std::optional<std::filesystem::path> NormalizeExecutablePath(std::filesystem::path candidate, const char* executableName)
		{
			if (candidate.empty()) {
				return std::nullopt;
			}

			std::error_code ec;
			if (std::filesystem::is_directory(candidate, ec) && !ec) {
				candidate /= executableName;
			}

			ec.clear();
			if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
				return std::nullopt;
			}

			ec.clear();
			const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
			return ec ? candidate.lexically_normal() : canonical;
		}

		std::optional<std::filesystem::path> GetEnvironmentExecutablePath(const char* variableName, const char* executableName)
		{
			const char* value = std::getenv(variableName);
			if (value == nullptr || value[0] == '\0') {
				return std::nullopt;
			}

			return NormalizeExecutablePath(std::filesystem::path(value), executableName);
		}

		std::optional<std::filesystem::path> SearchExecutableOnPath(const char* executableName)
		{
			char stackBuffer[MAX_PATH]{};
			char* filePart = nullptr;
			DWORD length = SearchPathA(nullptr, executableName, nullptr, MAX_PATH, stackBuffer, &filePart);
			if (length > 0 && length < MAX_PATH) {
				return NormalizeExecutablePath(std::filesystem::path(stackBuffer), executableName);
			}

			if (length >= MAX_PATH) {
				std::string dynamicBuffer(static_cast<size_t>(length) + 1, '\0');
				length = SearchPathA(nullptr, executableName, nullptr,
					static_cast<DWORD>(dynamicBuffer.size()), dynamicBuffer.data(), &filePart);
				if (length > 0 && length < dynamicBuffer.size()) {
					dynamicBuffer.resize(length);
					return NormalizeExecutablePath(std::filesystem::path(dynamicBuffer), executableName);
				}
			}

			return std::nullopt;
		}

		void AppendCMakeCandidate(std::vector<std::filesystem::path>& candidates,
			const char* environmentVariable,
			const std::filesystem::path& relativePath)
		{
			const char* root = std::getenv(environmentVariable);
			if (root == nullptr || root[0] == '\0') {
				return;
			}

			candidates.emplace_back(std::filesystem::path(root) / relativePath);
		}

		std::optional<std::filesystem::path> ResolveVisualStudioCMakeWithVswhere()
		{
			const char* programFilesX86 = std::getenv("ProgramFiles(x86)");
			if (programFilesX86 == nullptr || programFilesX86[0] == '\0') {
				return std::nullopt;
			}

			const std::filesystem::path vswherePath =
				std::filesystem::path(programFilesX86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
			auto normalizedVswhere = NormalizeExecutablePath(vswherePath, "vswhere.exe");
			if (!normalizedVswhere) {
				return std::nullopt;
			}

			Process::Result result = Process::Run({
				normalizedVswhere->string(),
				"-latest",
				"-requires", "Microsoft.VisualStudio.Component.VC.CMake.Project",
				"-property", "installationPath"
			});

			if (!result.Succeeded()) {
				return std::nullopt;
			}

			const std::string installPath = TrimWhitespace(result.Output);
			if (installPath.empty()) {
				return std::nullopt;
			}

			return NormalizeExecutablePath(
				std::filesystem::path(installPath) / "Common7" / "IDE" / "CommonExtensions" /
					"Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe",
				"cmake.exe");
		}

		std::optional<std::string> ResolveCMakeExecutable()
		{
			static std::optional<std::string> cachedCMakeExecutable;
			if (cachedCMakeExecutable) {
				return cachedCMakeExecutable;
			}

			for (const char* variableName : { "INDEX_CMAKE_PATH", "CMAKE_COMMAND", "CMAKE_EXE" }) {
				if (auto path = GetEnvironmentExecutablePath(variableName, "cmake.exe")) {
					cachedCMakeExecutable = path->string();
					return cachedCMakeExecutable;
				}
			}

			if (auto path = SearchExecutableOnPath("cmake.exe")) {
				cachedCMakeExecutable = path->string();
				return cachedCMakeExecutable;
			}

			if (auto path = ResolveVisualStudioCMakeWithVswhere()) {
				cachedCMakeExecutable = path->string();
				return cachedCMakeExecutable;
			}

			std::vector<std::filesystem::path> candidates;
			AppendCMakeCandidate(candidates, "ProgramFiles", std::filesystem::path("CMake") / "bin" / "cmake.exe");
			AppendCMakeCandidate(candidates, "ProgramFiles(x86)", std::filesystem::path("CMake") / "bin" / "cmake.exe");

			for (const char* version : { "2022", "2019" }) {
				for (const char* edition : { "Enterprise", "Professional", "Community", "BuildTools" }) {
					AppendCMakeCandidate(candidates, "ProgramFiles",
						std::filesystem::path("Microsoft Visual Studio") / version / edition / "Common7" /
							"IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe");
					AppendCMakeCandidate(candidates, "ProgramFiles(x86)",
						std::filesystem::path("Microsoft Visual Studio") / version / edition / "Common7" /
							"IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe");
				}
			}

			for (const std::filesystem::path& candidate : candidates) {
				if (auto path = NormalizeExecutablePath(candidate, "cmake.exe")) {
					cachedCMakeExecutable = path->string();
					return cachedCMakeExecutable;
				}
			}

			return std::nullopt;
		}
#else
		std::optional<std::string> ResolveCMakeExecutable()
		{
			return std::string("cmake");
		}
#endif

		std::vector<std::string> GetManagedWatchPatterns()
		{
			return {
				".cs",
				".csproj",
				".props",
				".targets",
				".sln"
			};
		}

		std::vector<std::string> BuildManagedWatchTargets(
			const std::filesystem::path& scriptsDirectory,
			const std::filesystem::path& projectFile,
			const std::filesystem::path& solutionFile = {})
		{
			std::vector<std::string> targets;
			auto appendTarget = [&targets](const std::filesystem::path& path) {
				if (!path.empty()) {
					targets.push_back(path.string());
				}
			};

			appendTarget(scriptsDirectory);
			appendTarget(projectFile);
			appendTarget(solutionFile);

			const std::filesystem::path projectRoot = projectFile.parent_path();
			if (!projectRoot.empty()) {
				appendTarget(projectRoot / "Directory.Build.props");
				appendTarget(projectRoot / "Directory.Build.targets");
			}

			return targets;
		}

		std::vector<std::string> GetNativeWatchPatterns()
		{
			return {
				".c",
				".cc",
				".cpp",
				".cxx",
				".h",
				".hh",
				".hpp",
				".hxx",
				".inl",
				".ipp"
			};
		}

		bool IsNativeScriptBootstrapFile(const std::filesystem::path& path)
		{
			return path.filename().string() == "NativeScriptExports.cpp";
		}

		bool IsNativeScriptSourceExtension(const std::filesystem::path& path)
		{
			std::string extension = path.extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});

			return extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx";
		}

		bool HasNativeScriptSourceFiles(const std::filesystem::path& sourceDirectory)
		{
			std::error_code ec;
			if (sourceDirectory.empty() || !std::filesystem::exists(sourceDirectory, ec) || ec) {
				return false;
			}

			for (std::filesystem::recursive_directory_iterator it(sourceDirectory, std::filesystem::directory_options::skip_permission_denied, ec), end;
				 it != end;
				 it.increment(ec)) {
				if (ec) {
					ec.clear();
					continue;
				}

				if (!it->is_regular_file(ec) || ec) {
					ec.clear();
					continue;
				}

				const std::filesystem::path path = it->path();
				if (IsNativeScriptBootstrapFile(path)) {
					continue;
				}

				if (IsNativeScriptSourceExtension(path)) {
					return true;
				}
			}

			return false;
		}

		std::vector<std::string> BuildNativeWatchTargets(
			const std::filesystem::path& projectDirectory,
			const std::filesystem::path& sourceDirectory = {})
		{
			std::vector<std::string> targets;
			auto appendTarget = [&targets](const std::filesystem::path& path) {
				if (!path.empty()) {
					targets.push_back(path.string());
				}
			};

			appendTarget(sourceDirectory);
			appendTarget(projectDirectory / "Include");

			if (targets.empty()) {
				appendTarget(projectDirectory);
			}

			return targets;
		}

		bool ShouldIgnoreBuildInputDirectory(const std::filesystem::path& directoryPath)
		{
			const std::string name = ToLowerCopy(directoryPath.filename().string());
			return name == ".git"
				|| name == ".vs"
				|| name == ".idea"
				|| name == "bin"
				|| name == "bin-int"
				|| name == "obj"
				|| name == "build"
				|| name == "out";
		}

		bool MatchesBuildInputPattern(const std::filesystem::path& filePath, const std::vector<std::string>& patterns)
		{
			if (patterns.empty()) {
				return true;
			}

			const std::string fileName = ToLowerCopy(filePath.filename().string());
			const std::string extension = ToLowerCopy(filePath.extension().string());

			for (const std::string& pattern : patterns) {
				const std::string normalized = ToLowerCopy(pattern);
				if (normalized.empty()) {
					continue;
				}

				if (normalized.front() == '.') {
					if (extension == normalized) {
						return true;
					}
				}
				else if (fileName == normalized) {
					return true;
				}
			}

			return false;
		}

		void UpdateLatestBuildInputTime(
			const std::filesystem::path& filePath,
			std::filesystem::file_time_type& latestWriteTime,
			bool& foundInput)
		{
			std::error_code ec;
			const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(filePath, ec);
			if (ec) {
				return;
			}

			if (!foundInput || writeTime > latestWriteTime) {
				latestWriteTime = writeTime;
				foundInput = true;
			}
		}

		bool TryGetLatestBuildInputTime(
			const std::vector<std::string>& targets,
			const std::vector<std::string>& patterns,
			std::filesystem::file_time_type& latestWriteTime)
		{
			bool foundInput = false;

			for (const std::string& targetString : targets) {
				if (targetString.empty()) {
					continue;
				}

				const std::filesystem::path target(targetString);
				std::error_code ec;
				if (std::filesystem::is_directory(target, ec) && !ec) {
					for (std::filesystem::recursive_directory_iterator it(target, std::filesystem::directory_options::skip_permission_denied, ec), end;
						 it != end;
						 it.increment(ec)) {
						if (ec) {
							ec.clear();
							continue;
						}

						if (it->is_directory(ec)) {
							if (!ec && ShouldIgnoreBuildInputDirectory(it->path())) {
								it.disable_recursion_pending();
							}
							ec.clear();
							continue;
						}

						if (!it->is_regular_file(ec) || ec || !MatchesBuildInputPattern(it->path(), patterns)) {
							ec.clear();
							continue;
						}

						UpdateLatestBuildInputTime(it->path(), latestWriteTime, foundInput);
					}

					continue;
				}

				ec.clear();
				if (!std::filesystem::is_regular_file(target, ec) || ec || !MatchesBuildInputPattern(target, patterns)) {
					continue;
				}

				UpdateLatestBuildInputTime(target, latestWriteTime, foundInput);
			}

			return foundInput;
		}

		bool BuildArtifactIsStale(
			const std::vector<std::string>& inputTargets,
			const std::vector<std::string>& inputPatterns,
			const std::filesystem::path& artifactPath)
		{
			std::filesystem::file_time_type latestInputWriteTime{};
			if (!TryGetLatestBuildInputTime(inputTargets, inputPatterns, latestInputWriteTime)) {
				return false;
			}

			std::error_code ec;
			if (artifactPath.empty() || !std::filesystem::exists(artifactPath, ec) || ec) {
				return true;
			}

			const std::filesystem::file_time_type artifactWriteTime = std::filesystem::last_write_time(artifactPath, ec);
			if (ec) {
				return true;
			}

			return latestInputWriteTime > artifactWriteTime;
		}

		template <typename TFunc>
		void ForEachLoadedScene(TFunc&& func)
		{
			Application* app = Application::GetInstance();
			if (!app || !app->GetSceneManager()) {
				return;
			}

			app->GetSceneManager()->ForeachLoadedScene(std::forward<TFunc>(func));
		}

		void RemovePendingFieldValuesForClass(ScriptComponent& scriptComp, const std::string& className)
		{
			if (className.empty()) {
				return;
			}

			for (const ScriptInstance& instance : scriptComp.Scripts) {
				if (instance.GetClassName() == className && instance.GetType() != ScriptType::Native) {
					return;
				}
			}

			const std::string prefix = className + ".";
			for (auto it = scriptComp.PendingFieldValues.begin(); it != scriptComp.PendingFieldValues.end(); ) {
				if (it->first.rfind(prefix, 0) == 0) {
					it = scriptComp.PendingFieldValues.erase(it);
				}
				else {
					++it;
				}
			}
			scriptComp.PendingDynamicComponentValues.erase(className);
		}

		std::vector<std::string> GetActiveGlobalSystemClassNames()
		{
			std::vector<std::string> classNames;
			IndexProject* project = ProjectManager::GetCurrentProject();
			if (!project) {
				return classNames;
			}

			for (const auto& registration : project->GlobalSystems) {
				if (registration.Active && !registration.ClassName.empty()) {
					classNames.push_back(registration.ClassName);
				}
			}
			return classNames;
		}

		void InitializeRegisteredGlobalSystems()
		{
			ScriptEngine::InitializeGlobalSystems(GetActiveGlobalSystemClassNames());
		}

		struct ScriptSceneScope {
			explicit ScriptSceneScope(Scene& scene)
				: Previous(ScriptEngine::GetScene())
			{
				ScriptEngine::SetScene(&scene);
			}

			~ScriptSceneScope()
			{
				ScriptEngine::SetScene(Previous);
			}

			Scene* Previous = nullptr;
		};

		uint64_t ToManagedEntityID(Scene& scene, EntityHandle entity)
		{
			// Prefer persistent UUID so C# `collision.OtherEntity == myCachedEntity` matches across creation paths.
			uint64_t entityID = static_cast<uint64_t>(static_cast<uint32_t>(entity));
			if (scene.IsValid(entity)) {
				const uint64_t persistentID = scene.GetEntityPersistentID(entity);
				if (persistentID != 0) {
					entityID = persistentID;
				}
			}
			return entityID;
		}

		struct ManagedScriptTeardownState {
			uint32_t Handle = 0;
			bool WasEnabled = false;
		};

		struct NativeScriptTeardownState {
			NativeScript* Instance = nullptr;
		};

		ManagedScriptTeardownState CaptureManagedTeardownState(const ScriptInstance& instance)
		{
			return {
				instance.GetGCHandle(),
				instance.HasEnabled()
			};
		}

		NativeScriptTeardownState CaptureNativeTeardownState(const ScriptInstance& instance)
		{
			return { instance.GetNativePtr() };
		}

		void InvokeManagedScriptTeardown(Scene& scene, const ManagedScriptTeardownState& state)
		{
			if (state.Handle == 0 || !ScriptEngine::IsInitialized()) {
				return;
			}

			ScriptSceneScope sceneScope(scene);
			if (state.WasEnabled) {
				ScriptEngine::InvokeOnDisable(state.Handle);
			}
			ScriptEngine::InvokeOnDestroy(state.Handle);
			ScriptEngine::DestroyScriptInstance(state.Handle);
		}

		void InvokeNativeScriptTeardown(Scene& scene, const NativeScriptTeardownState& state, NativeScriptHost& nativeHost)
		{
			if (!state.Instance) {
				return;
			}

			ScriptSceneScope sceneScope(scene);
			if (nativeHost.IsLoaded()) {
				nativeHost.DestroyInstance(state.Instance);
			}
		}

		void TeardownManagedScriptInstance(Scene& scene, ScriptInstance& instance)
		{
			if (!instance.HasManagedInstance()) {
				return;
			}

			// Unbind() before callbacks so the GCHandle is dropped even if IsInitialized() flips during teardown.
			const ManagedScriptTeardownState state = CaptureManagedTeardownState(instance);
			instance.Unbind();
			InvokeManagedScriptTeardown(scene, state);
		}

		void TeardownNativeScriptInstance(Scene& scene, ScriptInstance& instance, NativeScriptHost& nativeHost)
		{
			if (!instance.HasNativeInstance()) {
				return;
			}

			const NativeScriptTeardownState state = CaptureNativeTeardownState(instance);
			instance.Unbind();
			InvokeNativeScriptTeardown(scene, state, nativeHost);
		}

		template<typename Fn>
		bool TryInvokeNativeScript(const ScriptInstance& instance, const char* callbackName, Fn&& callback)
		{
			try {
				callback();
				return true;
			}
			catch (const std::exception& e) {
				IDX_CORE_ERROR_TAG("ScriptSystem", "Native script '{}' failed during {}: {}",
					instance.GetClassName(), callbackName, e.what());
			}
			catch (...) {
				IDX_CORE_ERROR_TAG("ScriptSystem", "Native script '{}' failed during {} with an unknown exception",
					instance.GetClassName(), callbackName);
			}

			return false;
		}

		bool IsTaskRunning(const std::shared_ptr<ProcessTaskState>& task)
		{
			return task && !task->Completed.load(std::memory_order_acquire);
		}

		std::shared_ptr<ProcessTaskState> LaunchTask(std::function<Process::Result()> work)
		{
			auto task = std::make_shared<ProcessTaskState>();
			task->StartedAt = std::chrono::steady_clock::now();

			// Capture by weak_ptr so abandoning during shutdown doesn't stall on the worker.
			std::weak_ptr<ProcessTaskState> weakTask = task;
			task->Worker = std::thread([weakTask, work = std::move(work)]() mutable {
				INDEX_PROFILE_THREAD_NAME("ScriptProcess");
				Process::Result result{};
				try {
					result = work();
				}
				catch (const std::exception& e) {
					result.ExitCode = -1;
					result.Output = e.what();
				}
				catch (...) {
					result.ExitCode = -1;
					result.Output = "Unknown exception while running background rebuild task.";
				}

				if (auto state = weakTask.lock()) {
					if (!state->Abandoned.load(std::memory_order_acquire)) {
						std::scoped_lock lock(state->Mutex);
						state->Result = std::move(result);
					}
					state->Completed.store(true, std::memory_order_release);
				}
			});

			return task;
		}

		bool TryTakeTaskResult(std::shared_ptr<ProcessTaskState>& task, Process::Result& outResult)
		{
			if (!task || !task->Completed.load(std::memory_order_acquire)) {
				return false;
			}

			if (task->Abandoned.load(std::memory_order_acquire)) {
				task.reset();
				return false;
			}

			{
				std::scoped_lock lock(task->Mutex);
				outResult = task->Result.value_or(Process::Result{});
			}

			task.reset();
			return true;
		}

		bool NativeBuildNeedsConfigure(const std::filesystem::path& nativeProjectDirectory, const std::filesystem::path& buildDirectory)
		{
			std::error_code ec;
			const std::filesystem::path cachePath = buildDirectory / "CMakeCache.txt";
			if (!std::filesystem::exists(cachePath, ec) || ec) {
				return true;
			}

			const std::filesystem::path cmakeListsPath = nativeProjectDirectory / "CMakeLists.txt";
			if (!std::filesystem::exists(cmakeListsPath, ec) || ec) {
				return false;
			}

			const auto cacheWriteTime = std::filesystem::last_write_time(cachePath, ec);
			if (ec) {
				return true;
			}

			const auto cmakeWriteTime = std::filesystem::last_write_time(cmakeListsPath, ec);
			if (ec) {
				return false;
			}

			return cmakeWriteTime > cacheWriteTime;
		}

		bool NativeBuildCacheExists(const std::filesystem::path& buildDirectory)
		{
			std::error_code ec;
			return std::filesystem::exists(buildDirectory / "CMakeCache.txt", ec) && !ec;
		}

		void AbandonTask(std::shared_ptr<ProcessTaskState>& task, std::string_view description)
		{
			if (!task) {
				return;
			}

			const bool stillRunning = !task->Completed.load(std::memory_order_acquire);
			if (stillRunning) {
				task->Abandoned.store(true, std::memory_order_release);
				IDX_CORE_WARN_TAG("ScriptSystem", "{} is still running during teardown; detaching so shutdown stays non-blocking", description);
			}

			task.reset();
		}

		// Snapshot before user callbacks run — callbacks can invalidate the EnTT view.
		std::vector<EntityHandle> CollectScriptEntities(Scene& scene)
		{
			std::vector<EntityHandle> result;
			auto view = scene.GetRegistry().view<ScriptComponent>(entt::exclude<DisabledTag>);
			result.reserve(view.size_hint());
			for (auto entity : view) {
				result.push_back(entity);
			}
			return result;
		}

		// Index-based walk with re-fetch each step; user callbacks can add/remove scripts or destroy the entity.
		template <typename Fn>
		void ForEachScriptOnEntitySafe(Scene& scene, EntityHandle entity, Fn&& fn)
		{
			for (size_t i = 0; ; ++i) {
				if (!scene.IsValid(entity) || !scene.HasComponent<ScriptComponent>(entity)) {
					return;
				}
				auto& scriptComp = scene.GetComponent<ScriptComponent>(entity);
				if (i >= scriptComp.Scripts.size()) {
					return;
				}
				if (!fn(scriptComp, scriptComp.Scripts[i], i)) {
					return;
				}
			}
		}

		ScriptInstance* FindManagedScriptInstance(Scene& scene, EntityHandle entity, uint32_t handle)
		{
			if (handle == 0 || !scene.IsValid(entity) || !scene.HasComponent<ScriptComponent>(entity)) {
				return nullptr;
			}

			auto& scripts = scene.GetComponent<ScriptComponent>(entity).Scripts;
			auto it = std::find_if(scripts.begin(), scripts.end(),
				[handle](const ScriptInstance& instance) { return instance.GetGCHandle() == handle; });
			return it != scripts.end() ? &(*it) : nullptr;
		}

		ScriptInstance* FindNativeScriptInstance(Scene& scene, EntityHandle entity, NativeScript* nativePtr)
		{
			if (nativePtr == nullptr || !scene.IsValid(entity) || !scene.HasComponent<ScriptComponent>(entity)) {
				return nullptr;
			}

			auto& scripts = scene.GetComponent<ScriptComponent>(entity).Scripts;
			auto it = std::find_if(scripts.begin(), scripts.end(),
				[nativePtr](const ScriptInstance& instance) { return instance.GetNativePtr() == nativePtr; });
			return it != scripts.end() ? &(*it) : nullptr;
		}

		// MUST run after GCHandle is set but before InvokeAwake/InvokeStart so OnAwake/OnStart see inspector-assigned values.
		// Read the live managed instance's current field values back into
		// PendingFieldValues so they survive a managed-assembly reload. Without
		// this, recompile resets editor-set fields to type-defaults: the values
		// live only on the instance after ApplyPendingFieldValues applied (and
		// erased) them, and the reload destroys the instance before the new one
		// is created. Capturing here also preserves runtime-modified fields.
		void SnapshotManagedFieldsToPending(ScriptComponent& scriptComp, const ScriptInstance& instance) {
			if (!instance.HasManagedInstance()) return;
			// Only capture in EDIT mode. This exists to preserve inspector-assigned
			// field values across an edit-mode recompile; doing it during PLAY would
			// copy RUNTIME-mutated values into the persistent PendingFieldValues, which
			// then bake into the pre-play snapshot and survive Stop — play-mode changes
			// must revert. (A play-mode hot-reload therefore re-applies authored values
			// rather than preserving runtime state, which is the correct trade-off.)
			if (Application::GetIsPlaying()) return;
			auto& callbacks = ScriptEngine::GetCallbacks();
			if (!callbacks.GetScriptFields) return;

			const char* rawJson = callbacks.GetScriptFields(static_cast<int32_t>(instance.GetGCHandle()));
			if (!rawJson || !*rawJson) return;

			Json::Value fields;
			if (!Json::TryParse(rawJson, fields, nullptr) || !fields.IsArray()) return;

			const std::string prefix = instance.GetClassName() + ".";
			for (const Json::Value& field : fields.GetArray()) {
				if (!field.IsObject()) continue;
				const Json::Value* nameNode = field.FindMember("name");
				const Json::Value* valueNode = field.FindMember("value");
				if (!nameNode || !valueNode) continue;
				const std::string name = nameNode->AsStringOr();
				if (name.empty()) continue;
				// Overwrite: the live instance is the freshest source — a stale
				// pending value (from a value applied earlier this session)
				// would otherwise shadow a runtime change.
				scriptComp.PendingFieldValues[prefix + name] = valueNode->AsStringOr();
			}
		}

		void ApplyPendingFieldValues(ScriptComponent& scriptComp, ScriptInstance& instance) {
			if (!instance.HasManagedInstance()) return;
			auto& callbacks = ScriptEngine::GetCallbacks();
			if (!callbacks.SetScriptField) return;

			const std::string prefix = instance.GetClassName() + ".";
			int applied = 0;
			int matchingPending = 0;
			for (auto it = scriptComp.PendingFieldValues.begin(); it != scriptComp.PendingFieldValues.end(); ) {
				if (it->first.rfind(prefix, 0) == 0) {
					++matchingPending;
					std::string fieldName = it->first.substr(prefix.size());
					callbacks.SetScriptField(
						static_cast<int32_t>(instance.GetGCHandle()),
						fieldName.c_str(), it->second.c_str());
					it = scriptComp.PendingFieldValues.erase(it);
					++applied;
				} else {
					++it;
				}
			}
			if (applied > 0 || matchingPending > 0) {
				IDX_CORE_INFO_TAG("ScriptSystem",
					"Applied {} pending field value(s) for {} (matched={})",
					applied, instance.GetClassName(), matchingPending);
			}
		}
	}

	void ScriptSystem::Awake(Scene& scene)
	{
		m_LastScene = &scene;

		// Skipped here so launcher doesn't lock Index-ScriptCore.dll while editor rebuilds it.
		auto* app = Application::GetInstance();
		if (app && !app->GetConfiguration().EnableScripting) {
			return;
		}

		++m_ActiveSystemCount;
		if (m_PollingOwner == nullptr) {
			m_PollingOwner = this;
		}
		if (m_ActiveSystemCount > 1) {
			// First scene's Awake handles engine/assembly init; subsequent scenes still fire OnAwake.
			if (ScriptEngine::IsInitialized())
			{
				const auto restoredCount = ScriptEngine::RestoreDynamicComponentsForScene(scene);
				if (restoredCount > 0) {
					IDX_INFO_TAG("ScriptSystem",
						"Restored {} dynamic component row(s) for scene '{}' before script Awake.",
						restoredCount, scene.GetName());
				}

				ScriptSceneScope sceneScope(scene);
				const auto entities = CollectScriptEntities(scene);
				for (EntityHandle entity : entities)
				{
					ForEachScriptOnEntitySafe(scene, entity,
						[entity](ScriptComponent& scriptComp, ScriptInstance& instance, size_t) -> bool {
							if (instance.GetClassName().empty()) return true;
							if (instance.GetType() == ScriptType::Native) return true;

							if (!instance.IsBound()) {
								instance.Bind(entity);
							}
							if (!instance.HasManagedInstance() && ScriptEngine::ClassExists(instance.GetClassName()))
							{
								uint32_t handle = ScriptEngine::CreateScriptInstance(instance.GetClassName(), entity);
								if (handle != 0) {
									instance.SetGCHandle(handle);
								}
							}
							if (instance.HasManagedInstance()) {
								// Capture handle before apply/invoke — re-entrant AddScript reallocates Scripts and dangles `instance`.
								const uint32_t awakeHandle = instance.GetGCHandle();
								ApplyPendingFieldValues(scriptComp, instance);
								ScriptEngine::InvokeAwake(awakeHandle);
							}
							return true;
						});
				}
			}
			return;
		}

		auto exeDir = std::filesystem::path(Path::ExecutableDir());
		IndexProject* project = ProjectManager::GetCurrentProject();

		const bool engineWasFreshlyInitialized = !ScriptEngine::IsInitialized();
		if (engineWasFreshlyInitialized) {
			ScriptEngine::Init();
		}

		if (m_CoreAssemblyPath.empty())
		{
			std::vector<std::filesystem::path> candidates = {
				exeDir / ".." / "Index-ScriptCore" / "Index-ScriptCore.dll",  // dev layout
				exeDir / "Index-ScriptCore.dll",                              // packaged build
			};
			for (const auto& candidate : candidates) {
				if (std::filesystem::exists(candidate)) {
					m_CoreAssemblyPath = std::filesystem::canonical(candidate).string();
					break;
				}
			}
		}

		if (!ScriptEngine::IsInitialized() && std::filesystem::exists(m_CoreAssemblyPath))
			ScriptEngine::LoadCoreAssembly(m_CoreAssemblyPath);

		if (m_UserAssemblyPath.empty())
		{
			if (project)
			{
				auto userDll = std::filesystem::path(project->GetUserAssemblyOutputPath());
				m_UserAssemblyPath = userDll.string();
				if (std::filesystem::exists(userDll)) {
					m_UserAssemblyPath = std::filesystem::canonical(userDll).string();
				}
				else {
					// Scan bin/<config>/ subdirs: editor and runtime may be built with different configs, so the configured path may miss.
					std::filesystem::path binRoot = std::filesystem::path(project->RootDirectory) / "bin";
					std::error_code binEc;
					if (std::filesystem::exists(binRoot, binEc) && !binEc) {
						const std::string targetFile = project->Name + ".dll";
						for (const auto& entry : std::filesystem::directory_iterator(binRoot, binEc)) {
							std::error_code itEc;
							if (binEc) { binEc.clear(); break; }
							if (!entry.is_directory(itEc) || itEc) continue;
							const std::filesystem::path candidate = entry.path() / targetFile;
							if (std::filesystem::exists(candidate, itEc) && !itEc) {
								m_UserAssemblyPath = std::filesystem::canonical(candidate).string();
								IDX_INFO_TAG("ScriptSystem",
									"User assembly resolved via bin/ scan: {} (configured path '{}' did not exist)",
									m_UserAssemblyPath, userDll.string());
								break;
							}
						}
					}
				}
			}
			else
			{
				auto sandboxPath = exeDir / ".." / "Index-Sandbox" / "Index-Sandbox.dll";
				m_UserAssemblyPath = sandboxPath.string();
				if (std::filesystem::exists(sandboxPath))
					m_UserAssemblyPath = std::filesystem::canonical(sandboxPath).string();
			}
		}

		if (!ScriptEngine::HasUserAssembly() && !m_UserAssemblyPath.empty() && std::filesystem::exists(m_UserAssemblyPath)) {
			ScriptEngine::LoadUserAssembly(m_UserAssemblyPath);
		}
		InitializeRegisteredGlobalSystems();

		// Script/native hot-reload (filesystem watch + auto-rebuild on edit) is an
		// editor-only convenience that needs the dotnet/cmake toolchain. It runs only
		// in the editor host (Application::IsEditor(), checked in each branch below);
		// every standalone runtime loads the prebuilt assemblies/DLLs as-is and never
		// watches or rebuilds. Development builds skip it at runtime; shipped Dist
		// builds (INDEX_WITH_EDITOR=0) compile the watch/rebuild paths out entirely.
		// Assembly and native-DLL loading below are unaffected either way.
#if INDEX_WITH_EDITOR
		constexpr bool kEnableHotReload = true;
#else
		constexpr bool kEnableHotReload = false;
#endif

		if (project)
		{
			auto scriptsDir = std::filesystem::path(project->AssetsDirectory);
			auto csproj = std::filesystem::path(project->CsprojPath);
			if (kEnableHotReload && Application::IsEditor() && std::filesystem::exists(csproj))
			{
				std::vector<std::string> managedWatchTargets =
					BuildManagedWatchTargets(scriptsDir, csproj, std::filesystem::path(project->SlnPath));
				m_SandboxProjectPath = std::filesystem::canonical(csproj).string();
				m_ScriptWatcher.Watch(
					managedWatchTargets,
					GetManagedWatchPatterns(),
					[this]() { RebuildAndReloadScripts(); });

				if (BuildArtifactIsStale(managedWatchTargets, GetManagedWatchPatterns(), std::filesystem::path(m_UserAssemblyPath))) {
					RebuildAndReloadScripts();
				}
			}
		}
		else
		{
			auto sandboxProjectDir = exeDir / ".." / ".." / ".." / "Index-Sandbox";
			auto sandboxSourceDir = sandboxProjectDir / "Source";
			auto sandboxCsproj = sandboxProjectDir / "Index-Sandbox.csproj";
			if (kEnableHotReload && Application::IsEditor() && std::filesystem::exists(sandboxCsproj))
			{
				std::vector<std::string> managedWatchTargets =
					BuildManagedWatchTargets(sandboxSourceDir, sandboxCsproj);
				m_SandboxProjectPath = std::filesystem::canonical(sandboxCsproj).string();
				m_ScriptWatcher.Watch(
					managedWatchTargets,
					GetManagedWatchPatterns(),
					[this]() { RebuildAndReloadScripts(); });

				if (BuildArtifactIsStale(managedWatchTargets, GetManagedWatchPatterns(), std::filesystem::path(m_UserAssemblyPath))) {
					RebuildAndReloadScripts();
				}
			}
		}

		if (project)
		{
			const bool hasNativeScriptSources = project->HasNativeScriptSources();
			if (hasNativeScriptSources) {
				project->EnsureNativeScriptProjectFiles();
			}

			m_NativeProjectDirectory = NormalizeNativePath(project->NativeScriptsDir).string();
			m_NativeSourceDirectory = NormalizeNativePath(project->NativeSourceDir).string();
			const std::filesystem::path nativeDll = ResolveProjectNativeDLLPath(*project);
			m_NativeDLLPath = nativeDll.string();
			m_NativeTargetName = project->Name + "-NativeScripts";
			if (hasNativeScriptSources && std::filesystem::exists(nativeDll))
			{
				m_NativeHost.LoadDLL(m_NativeDLLPath);
			}
			if (kEnableHotReload && Application::IsEditor() && !m_NativeProjectDirectory.empty())
			{
				std::vector<std::string> nativeWatchTargets =
					BuildNativeWatchTargets(std::filesystem::path(m_NativeProjectDirectory), std::filesystem::path(m_NativeSourceDirectory));
				m_NativeWatcher.Watch(
					nativeWatchTargets,
					GetNativeWatchPatterns(),
					[this]() { RebuildAndReloadNativeScripts(); });

				if (hasNativeScriptSources && BuildArtifactIsStale(nativeWatchTargets, GetNativeWatchPatterns(), std::filesystem::path(m_NativeDLLPath))) {
					RebuildAndReloadNativeScripts();
				}
			}
		}
		else
		{
			auto nativeProjectDir = exeDir / ".." / ".." / ".." / "Index-NativeScripts";
			auto nativeSourceDir = nativeProjectDir / "Source";
			const bool hasNativeScriptSources = HasNativeScriptSourceFiles(nativeSourceDir);
			m_NativeProjectDirectory = NormalizeNativePath(nativeProjectDir).string();
			m_NativeSourceDirectory = NormalizeNativePath(nativeSourceDir).string();

			const std::filesystem::path nativeDll = ResolveStandaloneNativeDLLPath(exeDir);
			m_NativeDLLPath = nativeDll.string();
			m_NativeTargetName = "Index-NativeScripts";
			if (hasNativeScriptSources && std::filesystem::exists(nativeDll))
			{
				m_NativeHost.LoadDLL(m_NativeDLLPath);
			}
			if (kEnableHotReload && Application::IsEditor() && !m_NativeProjectDirectory.empty())
			{
				std::vector<std::string> nativeWatchTargets =
					BuildNativeWatchTargets(std::filesystem::path(m_NativeProjectDirectory), std::filesystem::path(m_NativeSourceDirectory));
				m_NativeWatcher.Watch(
					nativeWatchTargets,
					GetNativeWatchPatterns(),
					[this]() { RebuildAndReloadNativeScripts(); });

				if (hasNativeScriptSources && BuildArtifactIsStale(nativeWatchTargets, GetNativeWatchPatterns(), std::filesystem::path(m_NativeDLLPath))) {
					RebuildAndReloadNativeScripts();
				}
			}
		}

		if (engineWasFreshlyInitialized) {
			IDX_INFO_TAG("ScriptSystem", "Script system initialized");
		}

		// Eager OnAwake dispatch for first scene; C# side guards against double-fire on reload.
		if (ScriptEngine::IsInitialized())
		{
			const auto restoredCount = ScriptEngine::RestoreDynamicComponentsForScene(scene);
			if (restoredCount > 0) {
				IDX_INFO_TAG("ScriptSystem",
					"Restored {} dynamic component row(s) for scene '{}' before script Awake.",
					restoredCount, scene.GetName());
			}

			ScriptSceneScope sceneScope(scene);
			const auto entities = CollectScriptEntities(scene);
			for (EntityHandle entity : entities)
			{
				ForEachScriptOnEntitySafe(scene, entity,
					[entity](ScriptComponent& scriptComp, ScriptInstance& instance, size_t) -> bool {
						if (instance.GetClassName().empty()) return true;
						if (instance.GetType() == ScriptType::Native) return true;

						if (!instance.IsBound()) {
							instance.Bind(entity);
						}
						if (!instance.HasManagedInstance() && ScriptEngine::ClassExists(instance.GetClassName()))
						{
							uint32_t handle = ScriptEngine::CreateScriptInstance(instance.GetClassName(), entity);
							if (handle != 0) {
								instance.SetGCHandle(handle);
							}
						}
						if (instance.HasManagedInstance()) {
							// Apply BEFORE InvokeAwake so OnAwake sees fields; capture handle first — AddScript in OnAwake reallocates Scripts and dangles `instance`.
							const uint32_t awakeHandle = instance.GetGCHandle();
							ApplyPendingFieldValues(scriptComp, instance);
							ScriptEngine::InvokeAwake(awakeHandle);
						}
						return true;
					});
			}
		}
	}

	void ScriptSystem::RebuildAndReloadScripts()
	{
		// Drop rebuilds during shutdown — late watcher events would lock the .dll mid-teardown.
		if (Application::IsShuttingDown()) {
			m_RebuildQueued = false;
			return;
		}
		if (m_RebuildTask) {
			m_RebuildQueued = true;
			return;
		}
		if (m_SandboxProjectPath.empty()) {
			m_RebuildQueued = false;
			return;
		}

		IDX_INFO_TAG("ScriptSystem", "Rebuilding C# scripts...");
		m_LastRebuildFailed = false;
		const std::string sandboxProjectPath = m_SandboxProjectPath;
		// Snapshot IndexProject statics on the main thread so the worker
		// can't race with a project reload that mutates them mid-build (M9).
		const std::string buildConfig = IndexProject::GetActiveBuildConfiguration();
		const char* primarySymbol = Application::IsEditor() ? "INDEX_EDITOR" : "INDEX_BUILD";
		const std::string defineConstantsArg =
			"-p:DefineConstants=" + IndexProject::BuildManagedDefineConstants(primarySymbol);
		m_RebuildTask = LaunchTask([sandboxProjectPath, buildConfig, defineConstantsArg]() {
			// IndexCodegenEnabled=false: hot-reload must not regenerate CodegenComponents.cpp — the running engine has the old C++ layout and a size mismatch would crash on next ComponentTypes<T> init.
			return Process::Run({
				"dotnet",
				"build",
				sandboxProjectPath,
				"-c", buildConfig,
				"--nologo",
				"-v", "q",
				"-nodeReuse:false",
				"-p:UseSharedCompilation=false",
				"-p:IndexCodegenEnabled=false",
				defineConstantsArg
			}, {}, std::chrono::minutes(5));
		});
	}

	void ScriptSystem::RebuildAndReloadNativeScripts()
	{
		// See RebuildAndReloadScripts — same shutdown-window race.
		if (Application::IsShuttingDown()) {
			m_NativeRebuildQueued = false;
			return;
		}
		if (m_NativeRebuildTask) {
			m_NativeRebuildQueued = true;
			return;
		}
		if (m_NativeProjectDirectory.empty()) {
			m_NativeRebuildQueued = false;
			return;
		}

		IndexProject* project = ProjectManager::GetCurrentProject();
		const bool hasNativeScriptSources = project
			? project->HasNativeScriptSources()
			: HasNativeScriptSourceFiles(std::filesystem::path(m_NativeSourceDirectory));

		if (!hasNativeScriptSources) {
			m_LastRebuildFailed = false;
			if (m_NativeHost.IsLoaded()) {
				ForEachLoadedScene([this](Scene& loadedScene) { TeardownNativeScripts(loadedScene); });
				m_NativeHost.UnloadDLL();
				IDX_INFO_TAG("ScriptSystem", "Native script sources removed; unloaded native script DLL");
			}
			return;
		}

		if (project) {
			project->EnsureNativeScriptProjectFiles();
			m_NativeProjectDirectory = NormalizeNativePath(project->NativeScriptsDir).string();
			m_NativeSourceDirectory = NormalizeNativePath(project->NativeSourceDir).string();
			m_NativeTargetName = project->Name + "-NativeScripts";
		}

		IDX_INFO_TAG("ScriptSystem", "Rebuilding native scripts...");
		m_LastRebuildFailed = false;
		const std::string nativeProjectDirectory = m_NativeProjectDirectory;
		const std::string buildConfig = GetActiveNativeBuildConfig();
		const std::string nativeTargetName = m_NativeTargetName;
		m_NativeRebuildTask = LaunchTask([nativeProjectDirectory, buildConfig, nativeTargetName]() {
			const std::filesystem::path buildDirectory = std::filesystem::path(nativeProjectDirectory) / "build";
			const std::optional<std::string> cmakeExecutable = ResolveCMakeExecutable();
			if (!cmakeExecutable) {
				Process::Result result{};
				result.Output =
					"CMake executable was not found. Install CMake, install the Visual Studio C++ CMake tools, "
					"add cmake.exe to PATH, or set INDEX_CMAKE_PATH to the cmake.exe location.";
				return result;
			}

			Process::Result configureResult{};
			if (NativeBuildNeedsConfigure(nativeProjectDirectory, buildDirectory)) {
				std::vector<std::string> configureCommand = {
					*cmakeExecutable,
					"-B", buildDirectory.string(),
					"-S", nativeProjectDirectory,
					"-DCMAKE_BUILD_TYPE=" + buildConfig,
					// Editor hot-reload always uses DEVELOPMENT; Build panel overrides for ship builds.
					"-DINDEX_BUILD_PROFILE=DEVELOPMENT"
				};

#if defined(IDX_PLATFORM_WINDOWS)
				if (!NativeBuildCacheExists(buildDirectory)) {
					configureCommand.push_back("-G");
					configureCommand.push_back("Visual Studio 17 2022");
					configureCommand.push_back("-A");
					configureCommand.push_back("x64");
				}
#endif

				configureResult = Process::Run(configureCommand, nativeProjectDirectory);
				if (!configureResult.Succeeded()) {
					return configureResult;
				}
			}

			std::vector<std::string> buildCommand = {
				*cmakeExecutable,
				"--build", buildDirectory.string(),
				"--config", buildConfig
			};
			if (!nativeTargetName.empty()) {
				buildCommand.push_back("--target");
				buildCommand.push_back(nativeTargetName);
			}

			Process::Result buildResult = Process::Run(buildCommand, nativeProjectDirectory);
			if (!configureResult.Output.empty()) {
				buildResult.Output = configureResult.Output + buildResult.Output;
			}
			return buildResult;
		});
	}

	bool ScriptSystem::RequestRebuildAndReloadAll()
	{
		const bool wasRebuilding = IsRebuilding();
		m_LastRebuildFailed = false;

		RebuildAndReloadScripts();
		RebuildAndReloadNativeScripts();

		return IsRebuilding() || wasRebuilding;
	}

	bool ScriptSystem::IsRebuilding() const
	{
		return m_RebuildTask != nullptr || m_NativeRebuildTask != nullptr;
	}

	void ScriptSystem::TeardownManagedScripts(Scene& scene)
	{
		if (!ScriptEngine::IsInitialized()) {
			return;
		}

		// RAII guard defers any watcher-triggered rebuild until teardown completes, preventing LoadUserAssembly while live GCHandles exist.
		struct TeardownGuard {
			TeardownGuard()  { ScriptSystem::m_TeardownInProgress.store(true,  std::memory_order_release); }
			~TeardownGuard() { ScriptSystem::m_TeardownInProgress.store(false, std::memory_order_release); }
		} guard;

		std::vector<EntityHandle> entities;
		auto view = scene.GetRegistry().view<ScriptComponent>();
		entities.reserve(view.size()); // single-component view: size() is exact, not a hint
		for (auto entity : view) entities.push_back(entity);

		for (EntityHandle entity : entities) {
			if (!scene.GetRegistry().valid(entity)) continue;
			if (!scene.GetRegistry().all_of<ScriptComponent>(entity)) continue;

			std::vector<ManagedScriptTeardownState> scripts;
			auto& scriptComp = scene.GetRegistry().get<ScriptComponent>(entity);
			scripts.reserve(scriptComp.Scripts.size());
			for (const ScriptInstance& instance : scriptComp.Scripts) {
				if (instance.HasManagedInstance()) {
					// Snapshot live field values BEFORE teardown so a managed
					// reload (recompile) re-applies them instead of resetting
					// to type-defaults. See SnapshotManagedFieldsToPending.
					SnapshotManagedFieldsToPending(scriptComp, instance);
					scripts.push_back(CaptureManagedTeardownState(instance));
				}
			}

			for (const ManagedScriptTeardownState& script : scripts) {
				if (script.Handle == 0) continue;
				if (!scene.GetRegistry().valid(entity)) continue;
				if (scene.GetRegistry().all_of<ScriptComponent>(entity)) {
					auto& currentScripts = scene.GetRegistry().get<ScriptComponent>(entity).Scripts;
					if (auto it = std::find_if(currentScripts.begin(), currentScripts.end(),
						[handle = script.Handle](const ScriptInstance& instance) { return instance.GetGCHandle() == handle; });
						it != currentScripts.end()) {
						TeardownManagedScriptInstance(scene, *it);
						continue;
					}
				}
			}
		}

	}

	void ScriptSystem::TeardownNativeScripts(Scene& scene)
	{
		// Mirror TeardownManagedScripts — same race surface, different language.
		struct TeardownGuard {
			TeardownGuard()  { ScriptSystem::m_TeardownInProgress.store(true,  std::memory_order_release); }
			~TeardownGuard() { ScriptSystem::m_TeardownInProgress.store(false, std::memory_order_release); }
		} guard;

		std::vector<EntityHandle> entities;
		auto view = scene.GetRegistry().view<ScriptComponent>();
		entities.reserve(view.size()); // single-component view: size() is exact, not a hint
		for (auto entity : view) entities.push_back(entity);

		for (EntityHandle entity : entities) {
			if (!scene.GetRegistry().valid(entity)) continue;
			if (!scene.GetRegistry().all_of<ScriptComponent>(entity)) continue;

			std::vector<NativeScriptTeardownState> scripts;
			auto& scriptComp = scene.GetRegistry().get<ScriptComponent>(entity);
			scripts.reserve(scriptComp.Scripts.size());
			for (const ScriptInstance& instance : scriptComp.Scripts) {
				if (instance.HasNativeInstance()) {
					scripts.push_back(CaptureNativeTeardownState(instance));
				}
			}

			for (const NativeScriptTeardownState& script : scripts) {
				if (!script.Instance) continue;
				if (!scene.GetRegistry().valid(entity)) continue;
				if (scene.GetRegistry().all_of<ScriptComponent>(entity)) {
					auto& currentScripts = scene.GetRegistry().get<ScriptComponent>(entity).Scripts;
					if (auto it = std::find_if(currentScripts.begin(), currentScripts.end(),
						[ptr = script.Instance](const ScriptInstance& instance) { return instance.GetNativePtr() == ptr; });
						it != currentScripts.end()) {
						TeardownNativeScriptInstance(scene, *it, m_NativeHost);
						continue;
					}
				}
			}
		}

	}

	bool ScriptSystem::RemoveScript(Entity entity, size_t index)
	{
		Scene* scene = entity.GetScene();
		if (scene == nullptr) {
			return false;
		}

		if (!entity.HasComponent<ScriptComponent>()) {
			return false;
		}

		auto& scriptComp = entity.GetComponent<ScriptComponent>();
		if (index >= scriptComp.Scripts.size()) {
			return false;
		}

		const ScriptInstance& instance = scriptComp.Scripts[index];
		const std::string removedClassName = instance.GetClassName();
		const ManagedScriptTeardownState managedState = CaptureManagedTeardownState(instance);
		const NativeScriptTeardownState nativeState = CaptureNativeTeardownState(instance);

		scriptComp.Scripts.erase(scriptComp.Scripts.begin() + static_cast<ptrdiff_t>(index));
		RemovePendingFieldValuesForClass(scriptComp, removedClassName);

		// Also remove the paired DynamicComponentStorage row for IComponent C# structs; Scripts-only removal would leave it dangling.
		auto& componentRegistry = SceneManager::Get().GetComponentRegistry();
		if (const ComponentInfo* info = componentRegistry.FindBySerializedName(removedClassName)) {
			if (info->isDynamic && info->remove) {
				info->remove(entity);
			}
		}

		InvokeManagedScriptTeardown(*scene, managedState);
		InvokeNativeScriptTeardown(*scene, nativeState, m_NativeHost);

		scene->MarkDirty();
		return true;
	}

	void ScriptSystem::RemoveAllScripts(Entity entity)
	{
		if (entity.GetScene() == nullptr || !entity.HasComponent<ScriptComponent>()) {
			return;
		}

		auto& scriptComp = entity.GetComponent<ScriptComponent>();
		for (size_t index = scriptComp.Scripts.size(); index > 0; --index) {
			RemoveScript(entity, index - 1);
		}

		scriptComp.PendingFieldValues.clear();
	}

	void ScriptSystem::SetScriptsEnabled(Entity entity, bool enabled)
	{
		Scene* scene = entity.GetScene();
		if (scene == nullptr || !entity.HasComponent<ScriptComponent>() || !ScriptEngine::IsInitialized()) {
			return;
		}

		ScriptSceneScope sceneScope(*scene);
		const EntityHandle entityHandle = entity.GetHandle();
		ForEachScriptOnEntitySafe(*scene, entityHandle,
			[enabled](ScriptComponent&, ScriptInstance& instance, size_t) -> bool {
			if (!instance.HasManagedInstance()) {
				return true;
			}

			const uint32_t handle = instance.GetGCHandle();
			if (enabled && !instance.HasEnabled()) {
				instance.MarkEnabled();
				ScriptEngine::InvokeOnEnable(handle);
			}
			else if (!enabled && instance.HasEnabled()) {
				instance.MarkDisabled();
				ScriptEngine::InvokeOnDisable(handle);
			}
			return true;
		});
	}

	namespace {
		enum class CollisionDispatchPhase {
			Enter,
			Stay,
			Exit
		};

		void DispatchCollision2D(Scene& scene, const Collision2D& collision, CollisionDispatchPhase phase)
		{
			if (!ScriptEngine::IsInitialized()) {
				return;
			}

			const auto dispatchToEntity = [&](EntityHandle self, EntityHandle other) {
				if (!scene.IsValid(self) || !scene.HasComponent<ScriptComponent>(self) || scene.HasComponent<DisabledTag>(self)) {
					return;
				}

				ScriptSceneScope sceneScope(scene);
				const uint64_t selfID = ToManagedEntityID(scene, self);
				const uint64_t otherID = scene.IsValid(other) ? ToManagedEntityID(scene, other) : 0;
				const uint64_t entityAID = scene.IsValid(collision.entityA) ? ToManagedEntityID(scene, collision.entityA) : 0;
				const uint64_t entityBID = scene.IsValid(collision.entityB) ? ToManagedEntityID(scene, collision.entityB) : 0;

				// Index-based walk with re-fetch — collision callbacks are
				// user code that can AddScript (vector reallocation),
				// RemoveScript, or Destroy(self).
				ForEachScriptOnEntitySafe(scene, self,
					[&, phase](ScriptComponent&, ScriptInstance& instance, size_t) -> bool {
						if (!instance.HasManagedInstance() || !instance.HasEnabled()) {
							return true;
						}

						switch (phase) {
						case CollisionDispatchPhase::Enter:
							ScriptEngine::InvokeCollisionEnter2D(instance.GetGCHandle(), selfID, otherID, entityAID, entityBID, collision.contactPoint.x, collision.contactPoint.y);
							break;
						case CollisionDispatchPhase::Stay:
							ScriptEngine::InvokeCollisionStay2D(instance.GetGCHandle(), selfID, otherID, entityAID, entityBID, collision.contactPoint.x, collision.contactPoint.y);
							break;
						case CollisionDispatchPhase::Exit:
							ScriptEngine::InvokeCollisionExit2D(instance.GetGCHandle(), selfID, otherID, entityAID, entityBID, collision.contactPoint.x, collision.contactPoint.y);
							break;
						}
						return true;
					});
			};

			dispatchToEntity(collision.entityA, collision.entityB);
			if (collision.entityB != collision.entityA) {
				dispatchToEntity(collision.entityB, collision.entityA);
			}
		}
	}

	void ScriptSystem::DispatchCollisionEnter2D(Scene& scene, const Collision2D& collision)
	{
		DispatchCollision2D(scene, collision, CollisionDispatchPhase::Enter);
	}

	void ScriptSystem::DispatchCollisionStay2D(Scene& scene, const Collision2D& collision)
	{
		DispatchCollision2D(scene, collision, CollisionDispatchPhase::Stay);
	}

	void ScriptSystem::DispatchCollisionExit2D(Scene& scene, const Collision2D& collision)
	{
		DispatchCollision2D(scene, collision, CollisionDispatchPhase::Exit);
	}

	void ScriptSystem::OnPreRender(Scene& scene)
	{
		(void)scene;
		if (m_PollingOwner == nullptr) {
			m_PollingOwner = this;
		}
		if (m_PollingOwner != this) {
			return;
		}

		// Skip file watcher polling while a script is being created/renamed
		if (!m_SuppressRecompile && m_AutoRecompileEnabled) {
			m_ScriptWatcher.Poll(1.0f);
			m_NativeWatcher.Poll(1.0f);
		}

		bool anyRebuilding = false;

		Process::Result result;
		if (TryTakeTaskResult(m_RebuildTask, result))
		{
			if (result.Succeeded())
			{
				ForEachLoadedScene([this](Scene& loadedScene) { TeardownManagedScripts(loadedScene); });
				if (!m_UserAssemblyPath.empty() && std::filesystem::exists(m_UserAssemblyPath)) {
					ScriptEngine::LoadUserAssembly(m_UserAssemblyPath);
				}
				else {
					ScriptEngine::ReloadAssemblies();
				}
				InitializeRegisteredGlobalSystems();
				// Invalidate baked prefab templates: managed reload can shift component layouts (sizeof changes) that would corrupt cached emplaceFromBytes payloads.
				PrefabTemplateCache::Get().InvalidateAll();
				IDX_INFO_TAG("ScriptSystem", "C# scripts rebuilt and reloaded");
			}
			else
			{
				m_LastRebuildFailed = true;
				IDX_ERROR_TAG("ScriptSystem", "C# build failed (exit code {})", result.ExitCode);
				if (!result.Output.empty()) {
					static const std::regex s_ErrorPattern(R"(error CS\d+:)");
					static const std::regex s_WarningPattern(R"(warning CS\d+:)");

					std::istringstream stream(result.Output);
					std::string line;
					std::string otherLines;
					while (std::getline(stream, line)) {
						if (!line.empty() && line.back() == '\r') {
							line.pop_back();
						}
						if (line.empty()) {
							continue;
						}
						if (std::regex_search(line, s_ErrorPattern)) {
							IDX_ERROR_TAG("ScriptSystem", "{}", line);
						}
						else if (std::regex_search(line, s_WarningPattern)) {
							IDX_WARN_TAG("ScriptSystem", "{}", line);
						}
						else {
							if (!otherLines.empty()) {
								otherLines.push_back('\n');
							}
							otherLines.append(line);
						}
					}
					if (!otherLines.empty()) {
						IDX_TRACE_TAG("ScriptSystem", "dotnet build output:\n{}", otherLines);
					}
				}
			}

			if (m_RebuildQueued) {
				if (!m_TeardownInProgress.load(std::memory_order_acquire)) {
					m_RebuildQueued = false;
					RebuildAndReloadScripts();
				}
			}
		}

		if (TryTakeTaskResult(m_NativeRebuildTask, result))
		{
			if (result.Succeeded())
			{
				const std::filesystem::path exeDir = std::filesystem::path(Path::ExecutableDir());
				if (IndexProject* project = ProjectManager::GetCurrentProject()) {
					m_NativeDLLPath = ResolveProjectNativeDLLPath(*project).string();
				}
				else {
					m_NativeDLLPath = ResolveStandaloneNativeDLLPath(exeDir).string();
				}

				if (!std::filesystem::exists(m_NativeDLLPath)) {
					m_LastRebuildFailed = true;
					IDX_ERROR_TAG("ScriptSystem", "Native scripts built, but DLL was not found at '{}'", m_NativeDLLPath);
				}
				else {
					// Swap only after the replacement DLL exists so failed rebuilds keep the previous host alive.
					ForEachLoadedScene([this](Scene& loadedScene) { TeardownNativeScripts(loadedScene); });

					if (!m_NativeHost.LoadDLL(m_NativeDLLPath)) {
						m_LastRebuildFailed = true;
						IDX_ERROR_TAG("ScriptSystem", "Native scripts built, but failed to load '{}'", m_NativeDLLPath);
						IDX_WARN_TAG("ScriptSystem", "Keeping the previous native script DLL loaded; instances will be recreated on the next update.");
					}
					else {
						PrefabTemplateCache::Get().InvalidateAll();
						IDX_INFO_TAG("ScriptSystem", "Native scripts rebuilt and reloaded");
					}
				}
			}
			else
			{
				m_LastRebuildFailed = true;
				IDX_ERROR_TAG("ScriptSystem", "Native build failed (exit code {})", result.ExitCode);
				if (!result.Output.empty()) {
					IDX_ERROR_TAG("ScriptSystem", "{}", result.Output);
				}
			}

			if (m_NativeRebuildQueued) {
				// See managed branch above — same teardown race rationale.
				if (!m_TeardownInProgress.load(std::memory_order_acquire)) {
					m_NativeRebuildQueued = false;
					RebuildAndReloadNativeScripts();
				}
			}
		}

		(void)anyRebuilding;
	}

	bool ScriptSystem::IsScriptRebuildRunning() const
	{
		return IsTaskRunning(m_RebuildTask);
	}

	bool ScriptSystem::IsNativeRebuildRunning() const
	{
		return IsTaskRunning(m_NativeRebuildTask);
	}

	float ScriptSystem::GetActiveRebuildElapsedSeconds() const
	{
		const bool nativeRunning = IsTaskRunning(m_NativeRebuildTask);
		const bool managedRunning = IsTaskRunning(m_RebuildTask);
		if (!nativeRunning && !managedRunning) {
			return 0.0f;
		}

		const auto startTime = nativeRunning ? m_NativeRebuildTask->StartedAt : m_RebuildTask->StartedAt;
		return std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime).count();
	}

	void ScriptSystem::Update(Scene& scene)
	{
		INDEX_PROFILE_SCOPE("Scripts");
		m_LastScene = &scene;
		ScriptSceneScope sceneScope(scene);
		const bool managedRuntimeReady = ScriptEngine::IsInitialized();

		float dt = Application::GetInstance() ? Application::GetInstance()->GetTime().GetDeltaTime() : 0.0f;

		// Drain pending coroutine continuations BEFORE user OnUpdate runs.
		// An `await WaitForSeconds(0.1f)` whose timer expired last frame
		// resumes here, in the same logical slot as OnUpdate, matching
		// Unity's Update-then-Yield ordering.
		if (managedRuntimeReady) {
			ScriptEngine::PumpCoroutinesUpdate(dt);
		}

		// Snapshot: user Update() may add/remove ScriptComponents and entities.
		const auto entities = CollectScriptEntities(scene);

		for (EntityHandle entity : entities)
		{
			ForEachScriptOnEntitySafe(scene, entity,
				[&, entity](ScriptComponent& scriptCompRef, ScriptInstance& instanceRef, size_t) -> bool {
					// Use pointers so we can rebind after any user callback that reallocates ScriptComponent::Scripts.
					ScriptInstance* instance = &instanceRef;
					ScriptComponent* scriptComp = &scriptCompRef;
					(void)scriptComp;

					if (instance->GetClassName().empty()) return true;

					if (!instance->IsBound())
						instance->Bind(entity);

					if (!instance->HasAnyInstance())
					{
						const ScriptType scriptType = instance->GetType();
						const bool canUseManaged = scriptType == ScriptType::Managed || scriptType == ScriptType::Unknown;
						const bool canUseNative = scriptType == ScriptType::Native || scriptType == ScriptType::Unknown;

						if (canUseManaged && managedRuntimeReady && ScriptEngine::ClassExists(instance->GetClassName()))
						{
							uint32_t handle = ScriptEngine::CreateScriptInstance(
								instance->GetClassName(), entity);
							if (handle != 0)
								instance->SetGCHandle(handle);
						}
						if (!instance->HasAnyInstance()
							&& canUseNative
							&& !IsTaskRunning(m_NativeRebuildTask)
							&& m_NativeHost.IsLoaded())
						{
							NativeScript* native = m_NativeHost.CreateInstance(
								instance->GetClassName(), entity, &scene);
							if (native)
								instance->SetNativePtr(native);
						}

						if (!instance->HasAnyInstance())
							return true;

						ApplyPendingFieldValues(*scriptComp, *instance);
						if (instance->GetType() == ScriptType::Managed && instance->HasManagedInstance()) {
							const uint32_t awakeHandle = instance->GetGCHandle();
							ScriptEngine::InvokeAwake(awakeHandle);
							// Rebind: user Awake may have AddScript'd, reallocating Scripts.
							instance = FindManagedScriptInstance(scene, entity, awakeHandle);
							if (!instance) return true;
						}
					}

					if (instance->GetType() == ScriptType::Managed)
					{
						if (!managedRuntimeReady || !instance->HasManagedInstance()) {
							return true;
						}

						const uint32_t handle = instance->GetGCHandle();
						if (!instance->HasEnabled())
						{
							instance->MarkEnabled();
							ScriptEngine::InvokeOnEnable(handle);
							instance = FindManagedScriptInstance(scene, entity, handle);
							if (!instance) return true;
						}

						if (!instance->HasStarted())
						{
							instance->MarkStarted();
							ScriptEngine::InvokeStart(handle);
							instance = FindManagedScriptInstance(scene, entity, handle);
							if (!instance) return true;
						}
						ScriptEngine::InvokeUpdate(handle);
						// `instance` not used after this point; no rebind needed.
					}
					else if (instance->GetType() == ScriptType::Native)
					{
						if (IsTaskRunning(m_NativeRebuildTask) || !instance->HasNativeInstance()) {
							return true;
						}

						NativeScript* nativeInstance = instance->GetNativePtr();
						if (!nativeInstance) {
							instance->Unbind();
							return true;
						}

						if (!instance->HasStarted())
						{
							if (!TryInvokeNativeScript(*instance, "Start", [nativeInstance]() {
								nativeInstance->Start();
							})) {
								m_NativeHost.DestroyInstance(nativeInstance);
								// Rebind before Unbind — user Start may have realloc'd Scripts.
								instance = FindNativeScriptInstance(scene, entity, nativeInstance);
								if (instance) instance->Unbind();
								return true;
							}
							instance = FindNativeScriptInstance(scene, entity, nativeInstance);
							if (!instance) return true;
							instance->MarkStarted();
						}

						if (!TryInvokeNativeScript(*instance, "Update", [nativeInstance, dt]() {
							nativeInstance->Update(dt);
						})) {
							m_NativeHost.DestroyInstance(nativeInstance);
							instance = FindNativeScriptInstance(scene, entity, nativeInstance);
							if (instance) instance->Unbind();
							return true;
						}
					}
					return true;
				});
		}
	}

	// Skips bind/start bookkeeping; instance must already be Bound + Started by Update.
	void ScriptSystem::FixedUpdate(Scene& scene)
	{
		INDEX_PROFILE_SCOPE("Scripts.FixedUpdate");
		// ScriptSceneScope required: GetScene() is read by every NativeBindings call; wrong scene with additive scenes would silently no-op or null-deref.
		ScriptSceneScope sceneScope(scene);
		const bool managedRuntimeReady = ScriptEngine::IsInitialized();
		const float fixedDt = Application::GetInstance() ? Application::GetInstance()->GetTime().GetFixedDeltaTime() : 0.0f;

		// Drain WaitForFixedUpdate continuations before user OnFixedUpdate.
		if (managedRuntimeReady) {
			ScriptEngine::PumpCoroutinesFixedUpdate();
		}

		const auto entities = CollectScriptEntities(scene);
		for (EntityHandle entity : entities)
		{
			ForEachScriptOnEntitySafe(scene, entity,
				[&](ScriptComponent&, ScriptInstance& instance, size_t) -> bool {
					if (instance.GetClassName().empty() || !instance.IsBound() || !instance.HasStarted()) return true;

					if (instance.GetType() == ScriptType::Managed)
					{
						if (!managedRuntimeReady || !instance.HasManagedInstance()) return true;
						ScriptEngine::InvokeFixedUpdate(instance.GetGCHandle());
					}
					else if (instance.GetType() == ScriptType::Native)
					{
						if (IsTaskRunning(m_NativeRebuildTask) || !instance.HasNativeInstance()) return true;
						NativeScript* nativeInstance = instance.GetNativePtr();
						if (!nativeInstance) return true;
						if (!TryInvokeNativeScript(instance, "FixedUpdate", [nativeInstance, fixedDt]() {
							nativeInstance->FixedUpdate(fixedDt);
						})) {
							m_NativeHost.DestroyInstance(nativeInstance);
							// Rebind before Unbind — user FixedUpdate may have realloc'd Scripts.
							ScriptInstance* rebound = FindNativeScriptInstance(scene, entity, nativeInstance);
							if (rebound) rebound->Unbind();
						}
					}
					return true;
				});
		}
	}

	void ScriptSystem::OnDestroy(Scene& scene)
	{
		if (m_PollingOwner == this) {
			m_PollingOwner = nullptr;
		}

		if (ScriptEngine::IsInitialized()) {
			TeardownManagedScripts(scene);
		}
		TeardownNativeScripts(scene);

		if (m_ActiveSystemCount > 0) {
			--m_ActiveSystemCount;
		}
		if (m_ActiveSystemCount > 0) {
			return;
		}

		m_ScriptWatcher.Stop();
		m_NativeWatcher.Stop();

		AbandonTask(m_RebuildTask, "Managed script rebuild");
		AbandonTask(m_NativeRebuildTask, "Native script rebuild");
		m_RebuildQueued = false;
		m_NativeRebuildQueued = false;

		if (Application::IsShuttingDown() && ScriptEngine::IsInitialized()) {
			ScriptEngine::Shutdown();
		}

		m_NativeHost.UnloadDLL();
		m_LastScene = nullptr;
		m_CoreAssemblyPath.clear();
		m_UserAssemblyPath.clear();
		m_SandboxProjectPath.clear();
		m_NativeProjectDirectory.clear();
		m_NativeSourceDirectory.clear();
		m_NativeDLLPath.clear();
		m_NativeTargetName.clear();
		m_SuppressRecompile = false;
		m_PollingOwner = nullptr;
	}

} // namespace Index
