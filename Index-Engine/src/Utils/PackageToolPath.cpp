#include <pch.hpp>
#include "Utils/PackageToolPath.hpp"

#include "Serialization/Path.hpp"

#include <system_error>
#include <vector>

namespace Index::PackageTool {

	std::string ExecutableName() {
#if defined(IDX_PLATFORM_WINDOWS)
		return "Index-PackageTool.exe";
#else
		return "Index-PackageTool";
#endif
	}

	std::filesystem::path ResolveExecutable() {
		const std::filesystem::path exeDir(Path::ExecutableDir());

		std::vector<std::filesystem::path> candidates;
		candidates.push_back(exeDir / ExecutableName());
		candidates.push_back(exeDir / "Index-PackageTool.dll");

		const std::filesystem::path projectDir = exeDir / ".." / ".." / ".." / "Index-PackageTool";
		for (const char* configuration : { "Debug", "Release", "Dist" }) {
			const std::filesystem::path outputDirectory = projectDir / "bin" / configuration / "net9.0";
			candidates.push_back(outputDirectory / ExecutableName());
			candidates.push_back(outputDirectory / "Index-PackageTool.dll");
		}

		for (const auto& candidate : candidates) {
			std::error_code ec;
			if (std::filesystem::exists(candidate, ec) && !ec) {
				return std::filesystem::canonical(candidate, ec);
			}
		}
		return {};
	}

} // namespace Index::PackageTool
