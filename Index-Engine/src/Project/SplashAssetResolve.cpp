#include "pch.hpp"
#include "Project/SplashAssetResolve.hpp"

#include "Core/Version.hpp"
#include "Project/IndexProject.hpp"
#include "Serialization/Path.hpp"

#include <filesystem>

namespace Index::SplashAssetResolve {

	namespace {
		constexpr std::string_view k_IndexAssetsPrefix = "index:";

		bool StartsWith(const std::string& s, std::string_view prefix) {
			return s.size() >= prefix.size() && std::memcmp(s.data(), prefix.data(), prefix.size()) == 0;
		}

		std::string StripPrefix(const std::string& s, std::string_view prefix) {
			return s.substr(prefix.size());
		}

		// Case-insensitive prefix compare for path roots — Windows paths
		// can mix "C:/Users/foo" vs "C:\\Users\\Foo" with different casing on
		// the drive letter even though they refer to the same location.
		bool PathStartsWith(const std::filesystem::path& candidate, const std::filesystem::path& root) {
			auto canIt = candidate.begin();
			auto canEnd = candidate.end();
			auto rootIt = root.begin();
			auto rootEnd = root.end();
			for (; rootIt != rootEnd; ++rootIt, ++canIt) {
				if (canIt == canEnd) return false;
				if (rootIt->empty()) continue;
				if (rootIt->compare(*canIt) != 0) {
					// Fall back to case-insensitive compare for the drive
					// component on Windows — pure path::compare is byte-exact.
					const std::string a = rootIt->string();
					const std::string b = canIt->string();
					if (a.size() != b.size()) return false;
					for (size_t i = 0; i < a.size(); ++i) {
						if (std::tolower(static_cast<unsigned char>(a[i])) !=
							std::tolower(static_cast<unsigned char>(b[i]))) {
							return false;
						}
					}
				}
			}
			return true;
		}
	}

	std::string Resolve(const std::string& stored, const IndexProject* project) {
		if (stored.empty()) return {};

		// Engine-shipped IndexAssets — "index:Textures/icon.png" → <IndexAssets>/Textures/icon.png
		if (StartsWith(stored, k_IndexAssetsPrefix)) {
			const std::string sub = StripPrefix(stored, k_IndexAssetsPrefix);
			const std::string root = Path::ResolveIndexAssets("");
			if (root.empty()) return {};
			std::filesystem::path candidate = std::filesystem::path(root) / sub;
			if (std::filesystem::exists(candidate)) return candidate.string();
			return {};
		}

		// Absolute path — accept verbatim when it exists (non-portable, but
		// supports the "I dragged a file from outside the project" case).
		std::filesystem::path p(stored);
		if (p.is_absolute()) {
			return std::filesystem::exists(p) ? p.string() : std::string{};
		}

		if (!project) return {};

		// Project-relative ("Assets/foo.png" style).
		std::filesystem::path projectRel = std::filesystem::path(project->RootDirectory) / p;
		if (std::filesystem::exists(projectRel)) return projectRel.string();

		// Last-resort: project assets dir + bare filename. Mirrors the
		// fallback the runtime used historically when launched with a cwd
		// different from the project root.
		std::filesystem::path assetsRel = std::filesystem::path(project->AssetsDirectory) / p.filename();
		if (std::filesystem::exists(assetsRel)) return assetsRel.string();

		return {};
	}

	std::string NormalizeForStorage(const std::string& pickedAbsolute, const IndexProject* project) {
		if (pickedAbsolute.empty()) return {};

		std::error_code ec;
		std::filesystem::path absFs = std::filesystem::weakly_canonical(
			std::filesystem::path(pickedAbsolute), ec);
		if (ec) absFs = std::filesystem::path(pickedAbsolute);

		// Inside IndexAssets? Store as "index:<subpath>" so the path stays
		// machine-independent (different installs put IndexAssets in
		// different absolute locations) and the resolver can route through
		// Path::ResolveIndexAssets at load time.
		const std::string indexRootStr = Path::ResolveIndexAssets("");
		if (!indexRootStr.empty()) {
			std::filesystem::path indexRoot = std::filesystem::weakly_canonical(
				std::filesystem::path(indexRootStr), ec);
			if (ec) indexRoot = std::filesystem::path(indexRootStr);
			if (PathStartsWith(absFs, indexRoot)) {
				std::filesystem::path rel = std::filesystem::relative(absFs, indexRoot, ec);
				if (!ec && !rel.empty()) {
					std::string sub = rel.generic_string();
					return std::string(k_IndexAssetsPrefix) + sub;
				}
			}
		}

		// Inside the project's Assets/ tree? Store relative to the project
		// root (matches the existing "Assets/foo.png" convention).
		if (project) {
			std::filesystem::path assetsDir = std::filesystem::weakly_canonical(
				std::filesystem::path(project->AssetsDirectory), ec);
			if (ec) assetsDir = std::filesystem::path(project->AssetsDirectory);
			if (PathStartsWith(absFs, assetsDir)) {
				std::filesystem::path rel = std::filesystem::relative(absFs, assetsDir.parent_path(), ec);
				if (!ec && !rel.empty()) return rel.generic_string();
			}
		}

		// Outside both trees — fall back to bare filename. Same behaviour
		// the old inline picker code had; preserved so existing projects
		// don't regress, but it produces an unresolvable path at load
		// time (no portable anchor). The picker UI lists a warning when
		// this branch fires.
		return absFs.filename().string();
	}

	std::string DefaultLogoPath() {
		const std::string root = Path::ResolveIndexAssets("Textures");
		if (root.empty()) return {};
		std::filesystem::path candidate = std::filesystem::path(root) / "icon.png";
		if (std::filesystem::exists(candidate)) return candidate.string();
		candidate = std::filesystem::path(root) / "Index64.png";
		if (std::filesystem::exists(candidate)) return candidate.string();
		return {};
	}

	std::string DefaultSubtitleLine() {
		std::string profile;
#if defined(INDEX_BUILD_RELEASE)
		profile = "Release";
#elif defined(INDEX_BUILD_DEVELOPMENT)
		profile = "Development";
#else
		profile = "Development";
#endif
		std::string platform;
#if defined(IDX_PLATFORM_WINDOWS)
		platform = "Windows";
#elif defined(__APPLE__)
		platform = "macOS";
#else
		platform = "Linux";
#endif
		// "  ·  " uses U+00B7 (middle dot), inside the Latin-1 supplement
		// the engine's text renderer + ImGui's default font both bake by
		// default. Higher codepoints would need the on-demand SDF path.
		return std::string("Index ") + IDX_VERSION + "  \xC2\xB7  " + platform + "  \xC2\xB7  " + profile;
	}
}
