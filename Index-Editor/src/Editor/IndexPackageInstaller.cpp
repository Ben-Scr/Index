#include "Editor/IndexPackageInstaller.hpp"

#include "Core/Log.hpp"
#include "Project/IndexProject.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Path.hpp"
#include "Utils/Process.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace Index {

	namespace {

		// L13: per-fieldName regex pair, built once per (fieldName, quote-style)
		// pair on first request and cached for the rest of the process. The
		// previous version constructed two `std::regex` objects on every call;
		// over hundreds of fields × packages × layers that adds up on cold
		// package scans. Mirrors the pattern at lines below where the layer
		// detection regexes are file-scope statics.
		struct ExtractRegexPair {
			std::regex DoubleQuoted;
			std::regex SingleQuoted;
		};

		const ExtractRegexPair& GetExtractRegexes(const std::string& fieldName) {
			static std::unordered_map<std::string, ExtractRegexPair> s_Cache;
			static std::mutex s_CacheMutex;
			std::scoped_lock lock(s_CacheMutex);
			auto it = s_Cache.find(fieldName);
			if (it == s_Cache.end()) {
				ExtractRegexPair pair{
					std::regex(fieldName + "\\s*=\\s*\"([^\"]*)\""),
					std::regex(fieldName + "\\s*=\\s*'([^']*)'"),
				};
				it = s_Cache.emplace(fieldName, std::move(pair)).first;
			}
			return it->second;
		}

		std::string ExtractStringField(const std::string& content, const std::string& fieldName) {
			// Pattern: name = "value"  (= optionally surrounded by whitespace, value in double quotes)
			// Lua single-quote strings would also be valid; supporting both keeps things simple.
			// fieldName is treated as a literal token (assumed alphanumeric — which is true for
			// our schema fields: name / version / description), so no regex-escape needed.
			const ExtractRegexPair& regexes = GetExtractRegexes(fieldName);

			std::smatch match;
			if (std::regex_search(content, match, regexes.DoubleQuoted)) {
				return match[1].str();
			}
			if (std::regex_search(content, match, regexes.SingleQuoted)) {
				return match[1].str();
			}
			return {};
		}

		bool ReadFileToString(const std::filesystem::path& path, std::string& out) {
			std::ifstream stream(path, std::ios::binary);
			if (!stream.is_open()) {
				return false;
			}
			std::ostringstream ss;
			ss << stream.rdbuf();
			out = ss.str();
			return true;
		}

	} // namespace

	std::optional<IndexPackageManifest> IndexPackageInstaller::ReadManifest(const std::string& packageDir) {
		std::filesystem::path manifestPath = std::filesystem::path(packageDir) / "index-package.lua";
		std::error_code ec;
		if (!std::filesystem::exists(manifestPath, ec) || ec) {
			return std::nullopt;
		}

		std::string content;
		if (!ReadFileToString(manifestPath, content) || content.empty()) {
			return std::nullopt;
		}

		IndexPackageManifest manifest;
		manifest.Name = ExtractStringField(content, "name");
		manifest.Version = ExtractStringField(content, "version");
		manifest.Description = ExtractStringField(content, "description");
		manifest.PackageDir = std::filesystem::path(packageDir).generic_string();

		// Detect which layers are declared via `<layer> = {` patterns. Both canonical
		// names and legacy aliases are accepted; the loader normalizes them on the
		// premake side, but this editor parser used to only see the legacy names.
		static const std::regex k_NativeRe(R"((^|[\s,;{])native\s*=\s*\{)");
		static const std::regex k_EngineCoreRe(R"((^|[\s,;{])engine_core\s*=\s*\{)");
		static const std::regex k_NativeStandaloneRe(R"((^|[\s,;{])native_standalone\s*=\s*\{)");
		static const std::regex k_StandaloneCppRe(R"((^|[\s,;{])standalone_cpp\s*=\s*\{)");
		static const std::regex k_CSharpRe(R"((^|[\s,;{])csharp\s*=\s*\{)");

		manifest.HasNativeLayer = std::regex_search(content, k_NativeRe)
			|| std::regex_search(content, k_EngineCoreRe);
		manifest.HasNativeStandaloneLayer = std::regex_search(content, k_NativeStandaloneRe)
			|| std::regex_search(content, k_StandaloneCppRe);
		manifest.HasCSharpLayer = std::regex_search(content, k_CSharpRe);

		if (manifest.Name.empty() || manifest.Version.empty()) {
			return std::nullopt;
		}
		return manifest;
	}

	bool IndexPackageInstaller::ValidatePackageDir(const std::string& packageDir, std::string& outError) {
		std::filesystem::path manifestPath = std::filesystem::path(packageDir) / "index-package.lua";
		std::error_code ec;
		if (!std::filesystem::exists(manifestPath, ec) || ec) {
			outError = "index-package.lua not found in " + packageDir;
			return false;
		}

		auto manifest = ReadManifest(packageDir);
		if (!manifest) {
			outError = "index-package.lua at " + packageDir + " is missing required fields (name, version).";
			return false;
		}
		return true;
	}

	std::vector<IndexPackageManifest> IndexPackageInstaller::EnumeratePackages(const std::string& rootDir, bool isEngine) {
		std::vector<IndexPackageManifest> out;

		std::error_code ec;
		if (!std::filesystem::is_directory(rootDir, ec) || ec) {
			return out;
		}

		for (const auto& entry : std::filesystem::directory_iterator(rootDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
			if (ec) break;
			if (!entry.is_directory(ec) || ec) continue;
			auto manifest = ReadManifest(entry.path().generic_string());
			if (manifest) {
				manifest->IsEngine = isEngine;
				out.push_back(std::move(*manifest));
			}
		}

		std::sort(out.begin(), out.end(), [](const IndexPackageManifest& a, const IndexPackageManifest& b) {
			return a.Name < b.Name;
		});
		return out;
	}

	std::vector<IndexPackageManifest> IndexPackageInstaller::EnumerateAll(const std::string& projectRootDir) {
		std::vector<IndexPackageManifest> all;

		std::vector<std::filesystem::path> engineRootCandidates;
		auto appendRootCandidate = [&](std::filesystem::path root) {
			if (root.empty()) {
				return;
			}
			std::error_code ec;
			root = std::filesystem::weakly_canonical(root, ec);
			if (ec) {
				root = root.lexically_normal();
			}
			const auto it = std::find(engineRootCandidates.begin(), engineRootCandidates.end(), root);
			if (it == engineRootCandidates.end()) {
				engineRootCandidates.push_back(std::move(root));
			}
		};

		appendRootCandidate(IndexProject::GetEngineRootDir());
		appendRootCandidate(std::filesystem::current_path());
		appendRootCandidate(std::filesystem::path(Path::ExecutableDir()) / ".." / ".." / "..");

		std::vector<std::filesystem::path> scannedPackageRoots;
		auto appendPackagesFromRoot = [&](const std::filesystem::path& root, bool isEngine) {
			for (const char* packagesDirName : { "packages", "Packages" }) {
				std::filesystem::path packagesRoot = root / packagesDirName;
				std::error_code ec;
				std::filesystem::path key = std::filesystem::weakly_canonical(packagesRoot, ec);
				if (ec) {
					key = packagesRoot.lexically_normal();
				}
				if (std::find(scannedPackageRoots.begin(), scannedPackageRoots.end(), key) != scannedPackageRoots.end()) {
					continue;
				}
				scannedPackageRoots.push_back(key);

				auto manifests = EnumeratePackages(packagesRoot.generic_string(), isEngine);
				all.insert(all.end(), std::make_move_iterator(manifests.begin()), std::make_move_iterator(manifests.end()));
			}
		};

		for (const std::filesystem::path& engineRoot : engineRootCandidates) {
			appendPackagesFromRoot(engineRoot, true);
		}

		if (!projectRootDir.empty()) {
			appendPackagesFromRoot(std::filesystem::path(projectRootDir), false);
		}

		std::sort(all.begin(), all.end(), [](const IndexPackageManifest& a, const IndexPackageManifest& b) {
			if (a.IsEngine != b.IsEngine) {
				return a.IsEngine > b.IsEngine;
			}
			return a.Name < b.Name;
		});
		all.erase(std::unique(all.begin(), all.end(), [](const IndexPackageManifest& a, const IndexPackageManifest& b) {
			return a.Name == b.Name && a.IsEngine == b.IsEngine;
		}), all.end());

		return all;
	}

	IndexPackageInstaller::InstallResult IndexPackageInstaller::InstallFromGitHub(const std::string& url, const std::string& projectPackagesDir) {
		InstallResult result;

		if (url.empty()) {
			result.Message = "GitHub URL cannot be empty.";
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		// Derive a sane target directory name from the URL: strip trailing slash, take the
		// last path segment, drop a trailing ".git" if present.
		std::string repoName = url;
		while (!repoName.empty() && (repoName.back() == '/' || repoName.back() == '\\')) {
			repoName.pop_back();
		}
		const auto lastSlash = repoName.find_last_of("/\\");
		if (lastSlash != std::string::npos) {
			repoName = repoName.substr(lastSlash + 1);
		}
		if (repoName.size() >= 4 && repoName.compare(repoName.size() - 4, 4, ".git") == 0) {
			repoName = repoName.substr(0, repoName.size() - 4);
		}
		if (repoName.empty()) {
			result.Message = "Could not derive a repository name from URL: " + url;
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		std::filesystem::path targetDir = std::filesystem::path(projectPackagesDir) / repoName;

		std::error_code ec;
		if (std::filesystem::exists(targetDir, ec) && !ec) {
			result.Message = "A package with that name already exists at " + targetDir.generic_string();
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		std::filesystem::create_directories(projectPackagesDir, ec);

		IDX_INFO_TAG("IndexPackages", "Cloning {} into {} ...", url, targetDir.generic_string());

		Process::Result cloneResult = Process::Run({
			"git", "clone", "--depth", "1", url, targetDir.generic_string()
		});

		if (!cloneResult.Succeeded()) {
			std::error_code rmEc;
			std::filesystem::remove_all(targetDir, rmEc);
			result.Message = "git clone failed for " + url + ". " + cloneResult.Output;
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		std::string validateError;
		if (!ValidatePackageDir(targetDir.generic_string(), validateError)) {
			std::error_code rmEc;
			std::filesystem::remove_all(targetDir, rmEc);
			result.Message = "Cloned repo is not a valid Index package: " + validateError;
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		auto manifest = ReadManifest(targetDir.generic_string());
		result.PackageName = manifest ? manifest->Name : repoName;

		result.Success = true;
		result.Message = "Installed package '" + result.PackageName + "' from " + url;
		IDX_INFO_TAG("IndexPackages", "{}", result.Message);
		return result;
	}

	IndexPackageInstaller::InstallResult IndexPackageInstaller::InstallFromLocal(const std::string& srcDir, const std::string& projectPackagesDir) {
		InstallResult result;

		std::error_code ec;
		if (!std::filesystem::is_directory(srcDir, ec) || ec) {
			result.Message = "Source is not a directory: " + srcDir;
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		std::string validateError;
		if (!ValidatePackageDir(srcDir, validateError)) {
			result.Message = "Source is not a valid Index package: " + validateError;
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		const std::string dirName = std::filesystem::path(srcDir).filename().generic_string();
		if (dirName.empty()) {
			result.Message = "Could not derive a directory name from " + srcDir;
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		std::filesystem::path targetDir = std::filesystem::path(projectPackagesDir) / dirName;
		if (std::filesystem::exists(targetDir, ec) && !ec) {
			result.Message = "A package with that name already exists at " + targetDir.generic_string();
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		std::filesystem::create_directories(projectPackagesDir, ec);

		try {
			std::filesystem::copy(srcDir, targetDir, std::filesystem::copy_options::recursive);
		}
		catch (const std::exception& e) {
			std::error_code rmEc;
			std::filesystem::remove_all(targetDir, rmEc);
			result.Message = std::string("Copy failed: ") + e.what();
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		auto manifest = ReadManifest(targetDir.generic_string());
		result.PackageName = manifest ? manifest->Name : dirName;

		result.Success = true;
		result.Message = "Installed package '" + result.PackageName + "' from local directory " + srcDir;
		IDX_INFO_TAG("IndexPackages", "{}", result.Message);
		return result;
	}

	namespace {
		std::string BuildPackagesPropsContent(const IndexProject& project,
			const std::vector<IndexPackageManifest>& manifests) {
			std::string content;
			content += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
			content += "<!--\n";
			content += "  Auto-generated by Index Package Manager. Do not edit.\n";
			content += "  Imported from <Project>.csproj; lists each installed C# package as\n";
			content += "  a <Reference> so the user code can `using <PackageName>` it.\n";
			content += "-->\n";
			content += "<Project>\n";

			const std::string engineRoot = IndexProject::GetEngineRootDir();
			if (!engineRoot.empty()) {
				content += "  <PropertyGroup>\n";
				content += "    <IndexEngineRoot>" + engineRoot + "</IndexEngineRoot>\n";
				content += "  </PropertyGroup>\n";
			}

			content += "  <ItemGroup>\n";
			for (const std::string& packageName : project.Packages) {
				auto it = std::find_if(manifests.begin(), manifests.end(),
					[&](const IndexPackageManifest& m) { return m.Name == packageName; });
				if (it == manifests.end() || !it->HasCSharpLayer) {
					continue;
				}

				const std::string assemblyName = "Pkg." + packageName;

				if (it->IsEngine) {
					// Engine packages are built under <engine-root>/bin/<config>-windows-x86_64/Pkg.<Name>/.
					content += "    <Reference Include=\"" + assemblyName + "\">\n";
					content += "      <HintPath>$(IndexEngineRoot)\\bin\\$(Configuration)-windows-x86_64\\"
						+ assemblyName + "\\" + assemblyName + ".dll</HintPath>\n";
					content += "      <Private>true</Private>\n";
					content += "    </Reference>\n";
				}
				else {
					// Project-local packages also build in the engine bin tree (the loader
					// registers them in the same workspace), so the path is identical.
					content += "    <Reference Include=\"" + assemblyName + "\">\n";
					content += "      <HintPath>$(IndexEngineRoot)\\bin\\$(Configuration)-windows-x86_64\\"
						+ assemblyName + "\\" + assemblyName + ".dll</HintPath>\n";
					content += "      <Private>true</Private>\n";
					content += "    </Reference>\n";
				}
			}
			content += "  </ItemGroup>\n";
			content += "</Project>\n";
			return content;
		}

		// If the .csproj doesn't yet `<Import>` the IndexPackages.props file, splice it in
		// just before `</Project>`. Idempotent — does nothing if the import is already there.
		void EnsureCsprojImportsPackagesProps(const std::string& csprojPath) {
			if (!File::Exists(csprojPath)) {
				return;
			}
			std::string content = File::ReadAllText(csprojPath);
			if (content.find("IndexPackages.props") != std::string::npos) {
				return;
			}

			const std::string importLine =
				"  <Import Project=\"Packages/IndexPackages.props\" Condition=\"Exists('Packages/IndexPackages.props')\" />\n";

			const std::string closing = "</Project>";
			const auto pos = content.rfind(closing);
			if (pos == std::string::npos) {
				return;
			}

			content.insert(pos, importLine + "\n");
			if (!File::WriteAllText(csprojPath, content)) {
				IDX_ERROR_TAG("IndexPackages", "Failed to patch {} (write error)", csprojPath);
				return;
			}
			IDX_INFO_TAG("IndexPackages", "Patched {} to import IndexPackages.props", csprojPath);
		}
	}

	IndexPackageInstaller::InstallResult IndexPackageInstaller::InstallToProject(IndexProject& project, const std::string& packageName) {
		InstallResult result;

		if (packageName.empty()) {
			result.Message = "Package name cannot be empty.";
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		if (std::find(project.Packages.begin(), project.Packages.end(), packageName) != project.Packages.end()) {
			result.Success = true;
			result.Message = "Package '" + packageName + "' is already installed in this project.";
			IDX_INFO_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		auto allManifests = EnumerateAll(project.RootDirectory);
		auto manifestIt = std::find_if(allManifests.begin(), allManifests.end(),
			[&](const IndexPackageManifest& m) { return m.Name == packageName; });
		if (manifestIt == allManifests.end()) {
			result.Message = "Package '" + packageName +
				"' was not found in <engine>/packages/ or <project>/Packages/.";
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		project.Packages.push_back(packageName);
		project.Save();

		RegeneratePackageReferences(project);

		result.Success = true;
		result.Message = "Installed '" + packageName + "' to project '" + project.Name + "'";
		IDX_INFO_TAG("IndexPackages", "{}", result.Message);
		return result;
	}

	namespace {
		// After removing a package from the allow-list, sweep stale build outputs out
		// of the user project's bin tree so the directory reflects reality. Looks for
		// any file under <project>/bin/.../ whose name starts with "Pkg.<Name>." and
		// removes it. Best-effort: errors are logged but not propagated (a locked DLL
		// from a still-running editor instance is a normal case).
		void DeletePackageBinArtifacts(const IndexProject& project, const std::string& packageName) {
			std::filesystem::path binRoot = std::filesystem::path(project.RootDirectory) / "bin";
			std::error_code ec;
			if (!std::filesystem::is_directory(binRoot, ec) || ec) {
				return;
			}

			const std::string prefix = "Pkg." + packageName + ".";
			std::vector<std::filesystem::path> toRemove;

			std::error_code walkEc;
			for (auto it = std::filesystem::recursive_directory_iterator(
					binRoot,
					std::filesystem::directory_options::skip_permission_denied,
					walkEc);
				it != std::filesystem::recursive_directory_iterator();
				it.increment(walkEc)) {
				if (walkEc) break;
				const std::filesystem::directory_entry& entry = *it;
				std::error_code entryEc;
				if (!entry.is_regular_file(entryEc) || entryEc) continue;

				const std::string filename = entry.path().filename().generic_string();
				if (filename.size() <= prefix.size()) continue;
				if (filename.compare(0, prefix.size(), prefix) != 0) continue;

				toRemove.push_back(entry.path());
			}

			for (const auto& path : toRemove) {
				std::error_code rmEc;
				std::filesystem::remove(path, rmEc);
				if (rmEc) {
					IDX_WARN_TAG("IndexPackages", "Could not remove stale artifact '{}': {}",
						path.generic_string(), rmEc.message());
				}
				else {
					IDX_INFO_TAG("IndexPackages", "Removed stale package artifact: {}",
						path.generic_string());
				}
			}
		}
	}

	IndexPackageInstaller::InstallResult IndexPackageInstaller::UninstallFromProject(IndexProject& project, const std::string& packageName) {
		InstallResult result;

		auto it = std::find(project.Packages.begin(), project.Packages.end(), packageName);
		if (it == project.Packages.end()) {
			result.Success = true;
			result.Message = "Package '" + packageName + "' is not installed.";
			IDX_INFO_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		project.Packages.erase(it);
		project.Save();

		RegeneratePackageReferences(project);

		// Delete any Pkg.<Name>.dll / .pdb / .deps.json / .runtimeconfig.json copies
		// that dotnet build placed in the user project's bin tree. Without this, stale
		// bytes linger and confuse future loads.
		DeletePackageBinArtifacts(project, packageName);

		result.Success = true;
		result.Message = "Removed '" + packageName + "' from project '" + project.Name + "'";
		IDX_INFO_TAG("IndexPackages", "{}", result.Message);
		return result;
	}

	void IndexPackageInstaller::RegeneratePackageReferences(const IndexProject& project) {
		std::error_code ec;
		std::filesystem::create_directories(project.PackagesDirectory, ec);

		auto manifests = EnumerateAll(project.RootDirectory);
		const std::string content = BuildPackagesPropsContent(project, manifests);

		std::filesystem::path propsPath = std::filesystem::path(project.PackagesDirectory) / "IndexPackages.props";
		(void)File::WriteAllText(propsPath.generic_string(), content);

		EnsureCsprojImportsPackagesProps(project.CsprojPath);

		IDX_INFO_TAG("IndexPackages", "Regenerated {} ({} package(s))",
			propsPath.generic_string(), project.Packages.size());
	}

	IndexPackageInstaller::InstallResult IndexPackageInstaller::Uninstall(const std::string& projectPackagesDir, const std::string& packageName) {
		InstallResult result;

		std::filesystem::path targetDir = std::filesystem::path(projectPackagesDir) / packageName;
		std::error_code ec;
		if (!std::filesystem::exists(targetDir, ec) || ec) {
			result.Message = "Package directory not found: " + targetDir.generic_string();
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		const auto manifest = ReadManifest(targetDir.generic_string());
		if (!manifest) {
			// Refuse to delete arbitrary directories — only proceed if it's a real Index package.
			result.Message = "Refusing to remove '" + targetDir.generic_string() + "': no index-package.lua found.";
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		ec.clear();
		std::filesystem::remove_all(targetDir, ec);
		if (ec) {
			result.Message = "Failed to remove " + targetDir.generic_string() + ": " + ec.message();
			IDX_ERROR_TAG("IndexPackages", "{}", result.Message);
			return result;
		}

		result.Success = true;
		result.Message = "Removed package '" + packageName + "'";
		IDX_INFO_TAG("IndexPackages", "{}", result.Message);
		return result;
	}

}
