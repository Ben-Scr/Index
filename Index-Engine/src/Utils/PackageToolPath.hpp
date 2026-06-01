#pragma once

#include "Core/Export.hpp"

#include <filesystem>
#include <string>

namespace Index::PackageTool {

	// Filename of the Index-PackageTool executable for the current platform
	// ("Index-PackageTool.exe" on Windows, "Index-PackageTool" elsewhere).
	INDEX_API std::string ExecutableName();

	INDEX_API std::filesystem::path ResolveExecutable();

} // namespace Index::PackageTool
