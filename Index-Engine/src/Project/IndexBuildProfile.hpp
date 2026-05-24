#pragma once
#include "Core/Export.hpp"
#include "Project/IndexProject.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Index {

	// Target platform a build profile produces a runtime for. Kept separate
	// from IndexProject::RenderBackend (which is the platform's graphics
	// backend) so future platforms drop in by adding one enum value plus a
	// row in the BuildPlatformSupport table.
	//
	// Stable integer values — used as the serialization fallback when the
	// JSON file is hand-edited with a garbage platform string. Don't
	// renumber without a migration path for existing .indexbuild files.
	enum class BuildPlatform : uint8_t {
		Windows  = 0,  // display: "Windows Desktop" — kept as "Windows" for serialization back-compat
		Linux    = 1,
		MacOS    = 2,
		Android  = 3,
		IOS      = 4,
		VisionOS = 5,
		Web      = 6,
	};

	// A named bundle of (platform, render backend) the user can pick from
	// the Build panel. Persisted as a single-file `.indexbuild` JSON under
	// `<ProjectRoot>/BuildProfiles/`. The on-disk file stem is the profile's
	// identity (used by IndexProject::ActiveBuildProfileName); the `Name`
	// field is just a display label and can be renamed by re-saving under a
	// different filename.
	struct INDEX_API IndexBuildProfile {
		std::string Name;
		BuildPlatform Platform = BuildPlatform::Windows;
		IndexProject::RenderBackend RenderBackend = IndexProject::RenderBackend::Auto;

		// Stable serialization key written into the .indexbuild JSON.
		// "Windows" and "Linux" predate this expansion and are kept exactly
		// for back-compat with on-disk profiles.
		static const char* PlatformToString(BuildPlatform platform);
		static BuildPlatform PlatformFromString(std::string_view value);

		// Human-readable label shown in the editor UI. Differs from the
		// serialization key for platforms whose marketing name uses casing
		// or spacing the JSON key doesn't ("Windows" → "Windows Desktop",
		// "MacOS" → "macOS", "IOS" → "iOS", "VisionOS" → "visionOS").
		static const char* PlatformDisplayName(BuildPlatform platform);

		// Filename stem (no extension) of the platform's icon under
		// IndexAssets/Textures/Editor/PlatformIcons/. Returns nullptr when
		// no icon is shipped yet — caller should render a name-only row.
		static const char* PlatformIconName(BuildPlatform platform);

		// All platforms in display order. Used by the Build Profiles panel
		// to enumerate the target list without hand-maintaining a parallel
		// array.
		static std::vector<BuildPlatform> AllPlatforms();

		// Render backends the editor exposes as valid for a given platform.
		// Profiles loaded with a backend outside this list get coerced to
		// the first allowed entry on next save (and a warning is logged).
		static std::vector<IndexProject::RenderBackend> AllowedBackends(BuildPlatform platform);
		static bool IsBackendAllowed(BuildPlatform platform, IndexProject::RenderBackend backend);

		bool Save(const std::string& filePath) const;
		static IndexBuildProfile Load(const std::string& filePath);

		// Writes the engine's default profiles (Windows + Linux) into `directory`.
		// Files that already exist are left untouched so user edits are never
		// clobbered. The directory is created if it doesn't already exist.
		// Returns the number of profile files actually written.
		static int WriteDefaultProfiles(const std::string& directory);

		// File extension recognised by the editor (".indexbuild"). Centralised
		// so the panel and the Build flow use the same literal.
		static constexpr std::string_view FileExtension = ".indexbuild";
	};

} // namespace Index
