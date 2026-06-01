#pragma once

#include "Core/Export.hpp"

#include <string>
#include <vector>

namespace Index {

	struct LoadedPackage {
		std::string Name;          // Without the "Pkg." prefix or ".Native" suffix, e.g. "Index.Hello".
		std::string ModulePath;    // Absolute path to the loaded shared library.
		void*       ModuleHandle = nullptr; // HMODULE on Windows; void* dlopen handle on POSIX.
	};

	class INDEX_API PackageHost {
	public:
		// Scan and load all packages reachable from the current executable.
		// Idempotent — calling twice is a no-op.
		static void LoadAll();

		static size_t LoadInstalled();

		// Unload all packages in reverse order. Safe to call even if LoadAll() never ran.
		static void UnloadAll();

		static const std::vector<LoadedPackage>& GetLoaded();

		static bool IsPackageLoaded(const std::string& packageName);
	};

}
