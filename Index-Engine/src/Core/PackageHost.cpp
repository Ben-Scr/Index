#include "pch.hpp"
#include "Core/PackageHost.hpp"

#include "Core/Log.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/Path.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <unordered_set>

#ifdef IDX_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Index {

	namespace {
		std::vector<LoadedPackage> s_LoadedPackages;
		bool s_LoadAllRan = false;

		void* PlatformLoad(const std::string& path) {
#ifdef IDX_PLATFORM_WINDOWS
			return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
			return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
		}

		void* PlatformResolve(void* module, const char* symbol) {
#ifdef IDX_PLATFORM_WINDOWS
			return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(module), symbol));
#else
			return dlsym(module, symbol);
#endif
		}

		void PlatformUnload(void* module) {
#ifdef IDX_PLATFORM_WINDOWS
			FreeLibrary(static_cast<HMODULE>(module));
#else
			dlclose(module);
#endif
		}

		// SEH guard: stale-ABI package crashing in DllMain/static-init would kill the editor without this. __try/__except requires no local C++ destructors, hence the bare-pointer signature.
		void* LoadLibraryGuarded(const char* path) {
#ifdef IDX_PLATFORM_WINDOWS
			__try {
				return reinterpret_cast<void*>(::LoadLibraryA(path));
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
#else
			return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
		}

		// Same SEH rationale as LoadLibraryGuarded: OnLoad calls back into the engine and a stale ABI would crash the editor on every launch. *outCrashed distinguishes a trapped fault from a clean OnLoad return of 0.
		int InvokeOnLoadGuarded(void* moduleHandle, bool* outCrashed, bool* outHadExport) {
			*outCrashed = false;
			*outHadExport = false;
			using OnLoadFn = int (*)();
#ifdef IDX_PLATFORM_WINDOWS
			OnLoadFn fn = reinterpret_cast<OnLoadFn>(
				::GetProcAddress(static_cast<HMODULE>(moduleHandle), "IndexPackage_OnLoad"));
			if (!fn) return 0;
			*outHadExport = true;
			__try {
				return fn();
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				*outCrashed = true;
				return 0;
			}
#else
			OnLoadFn fn = reinterpret_cast<OnLoadFn>(dlsym(moduleHandle, "IndexPackage_OnLoad"));
			if (!fn) return 0;
			*outHadExport = true;
			return fn();
#endif
		}

		void InvokeOnUnloadGuarded(void* moduleHandle, bool* outCrashed) {
			*outCrashed = false;
			using OnUnloadFn = void (*)();
#ifdef IDX_PLATFORM_WINDOWS
			OnUnloadFn fn = reinterpret_cast<OnUnloadFn>(
				::GetProcAddress(static_cast<HMODULE>(moduleHandle), "IndexPackage_OnUnload"));
			if (!fn) return;
			__try {
				fn();
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				*outCrashed = true;
			}
#else
			OnUnloadFn fn = reinterpret_cast<OnUnloadFn>(dlsym(moduleHandle, "IndexPackage_OnUnload"));
			if (fn) fn();
#endif
		}

		std::string PackageNameFromFilename(const std::string& filename) {
			// "Pkg.Index.Hello.Native.dll"  ->  "Index.Hello"
			std::string stem = filename;
			const auto dot = stem.find_last_of('.');
			if (dot != std::string::npos) {
				stem = stem.substr(0, dot);
			}
			constexpr std::string_view k_Prefix = "Pkg.";
			constexpr std::string_view k_Suffix = ".Native";
			if (stem.rfind(k_Prefix, 0) == 0) {
				stem = stem.substr(k_Prefix.size());
			}
			if (stem.size() > k_Suffix.size() &&
				stem.compare(stem.size() - k_Suffix.size(), k_Suffix.size(), k_Suffix) == 0) {
				stem = stem.substr(0, stem.size() - k_Suffix.size());
			}
			return stem;
		}

		// Skips Pkg.<Name>/ (managed-only) folders: LoadLibrary-ing a .NET assembly pins it against C# hot-reload and produces false "no OnLoad export" warnings.
				bool LooksLikeNativePackageFolder(const std::string& folderName) {
			if (folderName.rfind("Pkg.", 0) != 0) return false;
			constexpr std::string_view k_NativeSuffix = ".Native";
			return folderName.size() > k_NativeSuffix.size()
				&& folderName.compare(folderName.size() - k_NativeSuffix.size(),
					k_NativeSuffix.size(), k_NativeSuffix) == 0;
		}

		void DiscoverIn(const std::filesystem::path& root, std::vector<std::filesystem::path>& outCandidates) {
			std::error_code ec;
			if (!std::filesystem::is_directory(root, ec) || ec) {
				return;
			}

			for (const auto& entry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
				if (ec) break;

				const auto& entryPath = entry.path();
				const std::string name = entryPath.filename().string();

				if (entry.is_directory(ec) && !ec) {
					if (LooksLikeNativePackageFolder(name)) {
#ifdef IDX_PLATFORM_WINDOWS
						const std::filesystem::path candidate = entryPath / (name + ".dll");
#else
						const std::filesystem::path candidate = entryPath / ("lib" + name + ".so");
#endif
						if (std::filesystem::exists(candidate, ec) && !ec) {
							outCandidates.push_back(candidate);
						}
					}
				}
			}
		}
	}

	namespace {
		size_t LoadInternal(bool isInitialLoadAll) {
			std::vector<std::filesystem::path> candidates;

			const std::filesystem::path exeDir(Path::ExecutableDir());
			// Dev / build layout: bin/<config>/<Consumer>/<exe>; package DLLs are siblings in
			// bin/<config>/Pkg.<Name>.Native/. Scan one level up from the exe directory.
			std::filesystem::path searchRoot = exeDir.parent_path();
			DiscoverIn(searchRoot, candidates);

			// Distribution layout (future): packages alongside the exe in a Packages/ folder.
			DiscoverIn(exeDir / "Packages", candidates);

			const IndexProject* projectForScan = ProjectManager::GetCurrentProject();
			if (projectForScan && !projectForScan->PackagesDirectory.empty()) {
				const std::filesystem::path projectPackages(projectForScan->PackagesDirectory);
				for (const std::string& packageName : projectForScan->Packages) {
					const std::filesystem::path candidate = projectPackages
						/ packageName
						/ "Bin" / "Windows-x64"
						/ ("Pkg." + packageName + ".Native.dll");
					std::error_code ec;
					if (std::filesystem::exists(candidate, ec) && !ec) {
						candidates.push_back(candidate);
					}
				}
			}

			// Only packages listed in the project's packages array are loaded; INDEX_LOAD_ALL_PACKAGES=1 overrides this for engine-dev smoke testing.
						const IndexProject* activeProject = ProjectManager::GetCurrentProject();
			std::unordered_set<std::string> allowList;
			if (activeProject) {
				allowList.insert(activeProject->Packages.begin(), activeProject->Packages.end());
			}

			bool loadEverything = false;
			if (const char* env = std::getenv("INDEX_LOAD_ALL_PACKAGES")) {
				if (env[0] == '1' || env[0] == 't' || env[0] == 'T') {
					loadEverything = true;
					if (isInitialLoadAll) {
						IDX_CORE_INFO_TAG("PackageHost",
							"INDEX_LOAD_ALL_PACKAGES is set — overriding the project's `packages` allow-list and loading every discovered package.");
					}
				}
			}

			if (!loadEverything && allowList.empty()) {
				if (isInitialLoadAll) {
					if (activeProject) {
						IDX_CORE_INFO_TAG("PackageHost",
							"No packages installed for project '{}' — skipping all {} discovered package DLL(s). Add entries to index-project.json's `packages` array to install them.",
							activeProject->Name, candidates.size());
					}
					else {
						IDX_CORE_INFO_TAG("PackageHost",
							"No project loaded — skipping all {} discovered package DLL(s). Pass --project=<path> or set INDEX_LOAD_ALL_PACKAGES=1 to load packages.",
							candidates.size());
					}
				}
				return 0;
			}

			std::unordered_set<std::string> alreadyLoadedByName;
			std::unordered_set<std::string> alreadyLoadedByPath;
			for (const LoadedPackage& existing : s_LoadedPackages) {
				alreadyLoadedByName.insert(existing.Name);
				alreadyLoadedByPath.insert(existing.ModulePath);
			}

			size_t newlyLoaded = 0;
			for (const auto& candidate : candidates) {
				const std::string pathStr = candidate.string();
				const std::string fileName = candidate.filename().string();
				const std::string packageName = PackageNameFromFilename(fileName);

				if (!loadEverything && allowList.find(packageName) == allowList.end()) {
					if (isInitialLoadAll) {
						IDX_CORE_INFO_TAG("PackageHost",
							"Skipping package '{}' — not in project's `packages` allow-list.", packageName);
					}
					continue;
				}

				if (alreadyLoadedByName.count(packageName) || alreadyLoadedByPath.count(pathStr)) {
					// Quietly skip — common during LoadInstalled() rescans.
					continue;
				}

				void* module = LoadLibraryGuarded(pathStr.c_str());
				if (!module) {
					IDX_CORE_WARN_TAG("PackageHost",
						"Failed to load package '{}' from '{}' (LoadLibrary returned null — likely binary "
						"incompatibility with the running engine, or a static-init crash). Skipping.",
						packageName, pathStr);
					continue;
				}

				bool onLoadCrashed = false;
				bool onLoadHadExport = false;
				const int onLoadResult = InvokeOnLoadGuarded(module, &onLoadCrashed, &onLoadHadExport);
				if (onLoadCrashed) {
					IDX_CORE_ERROR_TAG("PackageHost",
						"Package '{}' crashed during IndexPackage_OnLoad (likely binary incompatibility "
						"with the running engine — try uninstalling and reinstalling). Unloaded; editor continues.",
						packageName);
					PlatformUnload(module);
					continue;
				}
				if (onLoadHadExport && onLoadResult != 0) {
					IDX_CORE_WARN_TAG("PackageHost",
						"Package '{}' IndexPackage_OnLoad returned {} (non-zero); keeping module loaded.",
						packageName, onLoadResult);
				}
				else if (!onLoadHadExport) {
					IDX_CORE_INFO_TAG("PackageHost", "Loaded package '{}' (no IndexPackage_OnLoad export).", packageName);
				}

				LoadedPackage loaded;
				loaded.Name = packageName;
				loaded.ModulePath = pathStr;
				loaded.ModuleHandle = module;
				s_LoadedPackages.push_back(std::move(loaded));
				++newlyLoaded;
			}

			return newlyLoaded;
		}
	} // namespace

	void PackageHost::LoadAll() {
		if (s_LoadAllRan) {
			return;
		}

		const size_t loaded = LoadInternal(/*isInitialLoadAll=*/true);
		IDX_CORE_INFO_TAG("PackageHost", "Loaded {} package(s).", loaded);

		// Set the guard AFTER the load loop so a partial init (exception
		// thrown out of an OnLoad, etc.) leaves LoadAll callable again.
		s_LoadAllRan = true;
	}

	size_t PackageHost::LoadInstalled() {
		// No s_LoadAllRan check — this is the path that exists *because*
		// LoadAll already ran but didn't see the just-built DLL.
		const size_t loaded = LoadInternal(/*isInitialLoadAll=*/false);
		if (loaded > 0) {
			IDX_CORE_INFO_TAG("PackageHost",
				"LoadInstalled: loaded {} new package(s) (total now {}).",
				loaded, s_LoadedPackages.size());
		}
		return loaded;
	}

	void PackageHost::UnloadAll() {
		for (auto it = s_LoadedPackages.rbegin(); it != s_LoadedPackages.rend(); ++it) {
			bool onUnloadCrashed = false;
			InvokeOnUnloadGuarded(it->ModuleHandle, &onUnloadCrashed);
			if (onUnloadCrashed) {
				IDX_CORE_ERROR_TAG("PackageHost",
					"Package '{}' crashed during IndexPackage_OnUnload; continuing teardown.",
					it->Name);
			}
			PlatformUnload(it->ModuleHandle);
		}
		s_LoadedPackages.clear();
		s_LoadAllRan = false;
	}

	const std::vector<LoadedPackage>& PackageHost::GetLoaded() {
		return s_LoadedPackages;
	}

	bool PackageHost::IsPackageLoaded(const std::string& packageName) {
		for (const LoadedPackage& loaded : s_LoadedPackages) {
			if (loaded.Name == packageName) return true;
		}
		return false;
	}

}			
