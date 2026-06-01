#pragma once

#include "Core/Export.hpp"
#include "Packages/PackageInfo.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Index {

	// Parses installed packages from a .csproj; Index-ScriptCore is excluded (engine core, not a user package).
	// centralVersions supplies versions from Directory.Packages.props for PackageReferences without a Version attr.
	// Returns empty on parse failure — never throws — so the UI degrades gracefully on mid-edit files.
	INDEX_API std::vector<PackageInfo> ParseInstalledPackagesFromXml(
		std::string_view xmlText,
		const std::unordered_map<std::string, std::string>& centralVersions = {});

	// Walks the .csproj's directory upward, merging every Directory.Packages.props
	// it finds. The deepest props file wins (matches MSBuild's evaluation order).
	INDEX_API std::unordered_map<std::string, std::string> LoadCentralPackageVersionsFromDisk(
		const std::filesystem::path& csprojPath);

} // namespace Index
