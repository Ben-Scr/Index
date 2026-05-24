#include "pch.hpp"
#include "Project/IndexBuildProfile.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Core/Log.hpp"

#include <algorithm>
#include <filesystem>

namespace Index {

	const char* IndexBuildProfile::PlatformToString(BuildPlatform platform) {
		switch (platform) {
			case BuildPlatform::Windows:  return "Windows";
			case BuildPlatform::Linux:    return "Linux";
			case BuildPlatform::MacOS:    return "MacOS";
			case BuildPlatform::Android:  return "Android";
			case BuildPlatform::IOS:      return "iOS";
			case BuildPlatform::VisionOS: return "visionOS";
			case BuildPlatform::Web:      return "Web";
		}
		return "Windows";
	}

	BuildPlatform IndexBuildProfile::PlatformFromString(std::string_view value) {
		if (value == "Windows"  || value == "windows"  || value == "Win" || value == "win"
			|| value == "WindowsDesktop" || value == "Windows Desktop")                  return BuildPlatform::Windows;
		if (value == "Linux"    || value == "linux")                                     return BuildPlatform::Linux;
		if (value == "MacOS"    || value == "macOS"    || value == "macos"
			|| value == "OSX"   || value == "osx"      || value == "Mac")                return BuildPlatform::MacOS;
		if (value == "Android"  || value == "android")                                   return BuildPlatform::Android;
		if (value == "iOS"      || value == "IOS"      || value == "ios")                return BuildPlatform::IOS;
		if (value == "visionOS" || value == "VisionOS" || value == "visionos")           return BuildPlatform::VisionOS;
		if (value == "Web"      || value == "web"      || value == "HTML5" || value == "WebAssembly") return BuildPlatform::Web;
		return BuildPlatform::Windows;
	}

	const char* IndexBuildProfile::PlatformDisplayName(BuildPlatform platform) {
		switch (platform) {
			case BuildPlatform::Windows:  return "Windows Desktop";
			case BuildPlatform::Linux:    return "Linux";
			case BuildPlatform::MacOS:    return "macOS";
			case BuildPlatform::Android:  return "Android";
			case BuildPlatform::IOS:      return "iOS";
			case BuildPlatform::VisionOS: return "visionOS";
			case BuildPlatform::Web:      return "Web";
		}
		return "Windows Desktop";
	}

	const char* IndexBuildProfile::PlatformIconName(BuildPlatform platform) {
		switch (platform) {
			case BuildPlatform::Windows:  return "windows";
			case BuildPlatform::Linux:    return "linux";
			case BuildPlatform::MacOS:    return "macos";
			case BuildPlatform::Android:  return "android";
			case BuildPlatform::IOS:      return "ios";
			// visionOS/Web icons not shipped under PlatformIcons/ yet; the
			// panel renders a name-only row when this returns nullptr.
			case BuildPlatform::VisionOS: return nullptr;
			case BuildPlatform::Web:      return nullptr;
		}
		return nullptr;
	}

	std::vector<BuildPlatform> IndexBuildProfile::AllPlatforms() {
		return {
			BuildPlatform::Android,
			BuildPlatform::IOS,
			BuildPlatform::Linux,
			BuildPlatform::MacOS,
			BuildPlatform::VisionOS,
			BuildPlatform::Web,
			BuildPlatform::Windows,
		};
	}

	std::vector<IndexProject::RenderBackend> IndexBuildProfile::AllowedBackends(BuildPlatform platform) {
		switch (platform) {
			case BuildPlatform::Windows:
				return {
					IndexProject::RenderBackend::Direct3D12,
					IndexProject::RenderBackend::Direct3D11,
					IndexProject::RenderBackend::Vulkan,
					IndexProject::RenderBackend::OpenGL,
				};
			case BuildPlatform::Linux:
				return {
					IndexProject::RenderBackend::Vulkan,
					IndexProject::RenderBackend::OpenGL,
				};
			case BuildPlatform::MacOS:
			case BuildPlatform::IOS:
			case BuildPlatform::VisionOS:
				return {
					IndexProject::RenderBackend::Metal,
				};
			case BuildPlatform::Android:
				return {
					IndexProject::RenderBackend::Vulkan,
					IndexProject::RenderBackend::OpenGLES,
				};
			case BuildPlatform::Web:
				return {
					IndexProject::RenderBackend::OpenGLES,
				};
		}
		return {};
	}

	bool IndexBuildProfile::IsBackendAllowed(BuildPlatform platform, IndexProject::RenderBackend backend) {
		const auto allowed = AllowedBackends(platform);
		return std::find(allowed.begin(), allowed.end(), backend) != allowed.end();
	}

	bool IndexBuildProfile::Save(const std::string& filePath) const {
		Json::Value root = Json::Value::MakeObject();
		root.AddMember("name", Name);
		root.AddMember("platform", std::string(PlatformToString(Platform)));
		root.AddMember("renderBackend", std::string(IndexProject::RenderBackendToString(RenderBackend)));

		std::error_code ec;
		const std::filesystem::path target(filePath);
		if (target.has_parent_path()) {
			std::filesystem::create_directories(target.parent_path(), ec);
		}
		return File::WriteAllText(filePath, Json::Stringify(root, true));
	}

	IndexBuildProfile IndexBuildProfile::Load(const std::string& filePath) {
		IndexBuildProfile profile;
		profile.Name = std::filesystem::path(filePath).stem().string();

		if (!File::Exists(filePath)) {
			return profile;
		}

		Json::Value root;
		std::string parseError;
		const std::string text = File::ReadAllText(filePath);
		if (!Json::TryParse(text, root, &parseError) || !root.IsObject()) {
			IDX_WARN_TAG("Build", "Failed to parse build profile '{}': {} — using defaults.",
				filePath, parseError.empty() ? std::string("not a JSON object") : parseError);
			return profile;
		}

		if (const Json::Value* v = root.FindMember("name")) {
			std::string name = v->AsStringOr();
			if (!name.empty()) profile.Name = std::move(name);
		}
		if (const Json::Value* v = root.FindMember("platform")) {
			profile.Platform = PlatformFromString(v->AsStringOr("Windows"));
		}
		if (const Json::Value* v = root.FindMember("renderBackend")) {
			profile.RenderBackend = IndexProject::RenderBackendFromString(v->AsStringOr("Auto"));
		}

		// Forgiving validation: an out-of-matrix backend (e.g. D3D12 on a
		// Linux profile from a hand-edited file) snaps to the first allowed
		// entry so the editor never shows an illegal combo. The on-disk file
		// is rewritten with the corrected value on next save.
		if (!IsBackendAllowed(profile.Platform, profile.RenderBackend)) {
			const auto allowed = AllowedBackends(profile.Platform);
			if (!allowed.empty()) {
				IDX_WARN_TAG("Build",
					"Build profile '{}' has backend '{}' which isn't valid for platform '{}' — coercing to '{}'.",
					profile.Name,
					IndexProject::RenderBackendToString(profile.RenderBackend),
					PlatformToString(profile.Platform),
					IndexProject::RenderBackendToString(allowed.front()));
				profile.RenderBackend = allowed.front();
			}
		}
		return profile;
	}

	int IndexBuildProfile::WriteDefaultProfiles(const std::string& directory) {
		if (directory.empty()) return 0;

		std::error_code ec;
		std::filesystem::create_directories(directory, ec);

		// Mirror the panel's "New profile" defaults (Windows + Direct3D12) so
		// the seeded Windows profile matches what the user would otherwise get
		// from clicking New. Linux only has Vulkan available per
		// AllowedBackends(), so that's the only sensible pick there.
		struct DefaultSpec {
			const char* Name;
			BuildPlatform Platform;
			IndexProject::RenderBackend Backend;
		};
		const DefaultSpec defaults[] = {
			{ "Windows", BuildPlatform::Windows, IndexProject::RenderBackend::Direct3D12 },
			{ "Linux",   BuildPlatform::Linux,   IndexProject::RenderBackend::Vulkan     },
		};

		int written = 0;
		for (const auto& spec : defaults) {
			const std::filesystem::path target =
				std::filesystem::path(directory) /
				(std::string(spec.Name) + std::string(FileExtension));

			// Never clobber an existing file — preserves user edits if this
			// helper is ever invoked over a project that already has profiles.
			if (std::filesystem::exists(target, ec)) continue;

			IndexBuildProfile profile;
			profile.Name = spec.Name;
			profile.Platform = spec.Platform;
			profile.RenderBackend = spec.Backend;
			if (profile.Save(target.string())) {
				IDX_INFO_TAG("Build", "Seeded default build profile '{}' at '{}'.",
					profile.Name, target.string());
				++written;
			} else {
				IDX_WARN_TAG("Build", "Failed to write default build profile '{}' to '{}'.",
					profile.Name, target.string());
			}
		}
		return written;
	}

} // namespace Index
