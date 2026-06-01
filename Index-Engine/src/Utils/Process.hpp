#pragma once

#include "Core/Export.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace Index::Process {

	struct INDEX_API Result {
		int ExitCode = -1;
		std::string Output;
		bool TimedOut = false;

		bool Succeeded() const { return ExitCode == 0 && !TimedOut; }
	};

	// Run a child process synchronously. timeout::zero() waits forever (legacy default); positive timeout kills the child and sets TimedOut=true.
	INDEX_API Result Run(const std::vector<std::string>& command,
		const std::filesystem::path& workingDirectory = {},
		std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

	INDEX_API bool LaunchDetached(const std::vector<std::string>& command,
		const std::filesystem::path& workingDirectory = {});

} // namespace Index::Process
