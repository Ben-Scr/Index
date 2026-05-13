#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Index {

	// Lightweight projection of index-package.lua's top-level fields.
	// Used to populate the package-manager UI lists; not the full premake-loader manifest.
	struct IndexPackageManifest {
		std::string Name;          // "name" field — required
		std::string Version;       // "version" field — required
		std::string Description;   // "description" field — optional
		std::string PackageDir;    // Absolute path to the package directory
		bool        IsEngine = false; // true if under <engine-root>/packages/, else project-local

		// Which layers does this package declare? Detected by scanning the manifest
		// for `<layer> = {` patterns. At least one of these is true for any valid
		// manifest the loader will register. Both canonical names and the legacy
		// aliases (engine_core / standalone_cpp) are accepted by the parser.
		bool HasNativeLayer = false;            // canonical "native" (legacy alias: "engine_core")
		bool HasNativeStandaloneLayer = false;  // canonical "native_standalone" (legacy alias: "standalone_cpp")
		bool HasCSharpLayer = false;
	};

	// Install/validate helpers for index-package.lua-based packages.
	//
	// Distinct from `PackageManager` (which handles NuGet/GitHub *third-party* deps via the
	// Index-PackageTool CLI). This class operates on the engine's first-class package system:
	//   * "Engine packages" live under   <engine-root>/packages/<Name>/index-package.lua
	//   * "Project packages" live under  <project>/Packages/<Name>/index-package.lua
	class IndexPackageInstaller {
	public:
		struct InstallResult {
			bool        Success = false;
			std::string Message;
			// On successful GitHub/Local install, the package name parsed from the
			// cloned manifest. Caller can pass this to InstallToProject() to also add
			// the package to the project's allow-list.
			std::string PackageName;
		};

		// Read index-package.lua at `<packageDir>/index-package.lua` and project-relevant
		// fields. Returns nullopt if the file is missing or required fields (name, version)
		// are absent. Manifest parsing is pattern-based; not a full Lua interpreter.
		static std::optional<IndexPackageManifest> ReadManifest(const std::string& packageDir);

		// Convenience: returns true if `<packageDir>/index-package.lua` exists and parses.
		// If invalid, fills outError with a human-readable reason.
		static bool ValidatePackageDir(const std::string& packageDir, std::string& outError);

		// Enumerate every <rootDir>/<Name>/index-package.lua and return parsed manifests.
		// Skips entries that fail validation (silently — caller logs if it cares).
		static std::vector<IndexPackageManifest> EnumeratePackages(const std::string& rootDir, bool isEngine);

		// Convenience that scans both engine + project roots in one call.
		// projectRootDir may be empty (no project loaded).
		static std::vector<IndexPackageManifest> EnumerateAll(const std::string& projectRootDir = {});

		// Clone a GitHub repo into <projectPackagesDir>/<repoName>/. Validates the clone
		// has index-package.lua at root; on failure, removes the partial clone.
		static InstallResult InstallFromGitHub(const std::string& url, const std::string& projectPackagesDir);

		// Copy a local directory into <projectPackagesDir>/<dirName>/. Validates the source
		// is a valid Index package before copying.
		static InstallResult InstallFromLocal(const std::string& srcDir, const std::string& projectPackagesDir);

		// Remove a project-local package directory. Refuses to touch engine packages.
		static InstallResult Uninstall(const std::string& projectPackagesDir, const std::string& packageName);

		// Add a package name to project.Packages and persist index-project.json.
		// Validates the package exists either in <engine-root>/packages/ or
		// <project>/Packages/. After mutating the project, regenerates the
		// IndexPackages.props that the .csproj imports.
		// Caller is responsible for running premake regen + msbuild afterwards.
		static InstallResult InstallToProject(class IndexProject& project, const std::string& packageName);

		// Remove a package name from project.Packages, persist, and regenerate
		// IndexPackages.props. Does NOT delete the package's files on disk.
		static InstallResult UninstallFromProject(class IndexProject& project, const std::string& packageName);

		// Generate <project>/Packages/IndexPackages.props with one <Reference Include="...">
		// per installed C# package. Idempotent; safe to call repeatedly.
		// Also patches the project's .csproj to <Import> this props file if it doesn't already.
		static void RegeneratePackageReferences(const class IndexProject& project);
	};

}
