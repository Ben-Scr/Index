#pragma once
#include "Core/Export.hpp"
#include "Project/IndexProject.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Index {

	// Stable integer values — don't renumber without a migration path for existing .build files.
	enum class BuildPlatform : uint8_t {
		Windows  = 0,  // display: "Windows Desktop" — kept as "Windows" for serialization back-compat
		Linux    = 1,
		MacOS    = 2,
		Android  = 3,
		IOS      = 4,
		VisionOS = 5,
		Web      = 6,
	};

	struct INDEX_API IndexBuildProfile {
		std::string Name;
		BuildPlatform Platform = BuildPlatform::Windows;
		IndexProject::RenderBackend RenderBackend = IndexProject::RenderBackend::Auto;

		static const char* PlatformToString(BuildPlatform platform);
		static BuildPlatform PlatformFromString(std::string_view value);
		static const char* PlatformDisplayName(BuildPlatform platform);
		// Returns nullptr when no icon is shipped for this platform.
		static const char* PlatformIconName(BuildPlatform platform);
		static std::vector<BuildPlatform> AllPlatforms();
		// Profiles with a backend outside this list are coerced to the first allowed entry on save.
		static std::vector<IndexProject::RenderBackend> AllowedBackends(BuildPlatform platform);
		static bool IsBackendAllowed(BuildPlatform platform, IndexProject::RenderBackend backend);

		bool Save(const std::string& filePath) const;
		static IndexBuildProfile Load(const std::string& filePath);

		// Writes default profiles (Windows + Linux); skips files that already exist. Returns count written.
		static int WriteDefaultProfiles(const std::string& directory);

		static constexpr std::string_view FileExtension = ".build";
	};

} // namespace Index
