#pragma once

#include "Core/Export.hpp"

#include <filesystem>
#include <string>

namespace Index::PackageTool {

	// Filename of the Index-PackageTool executable for the current platform
	// ("Index-PackageTool.exe" on Windows, "Index-PackageTool" elsewhere).
	INDEX_API std::string ExecutableName();

	// Locate the Index-PackageTool binary the engine should invoke. Looks
	// alongside the running exe first (the postbuild copy lands the tool
	// next to consumer binaries), then falls back to the Index-PackageTool
	// build output a few levels up from the dev-layout exeDir. Returns
	// empty if nothing found.
	INDEX_API std::filesystem::path ResolveExecutable();

} // namespace Index::PackageTool
