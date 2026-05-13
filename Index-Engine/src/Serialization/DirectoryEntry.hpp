#pragma once

#include <string>

namespace Index {

	struct DirectoryEntry {
		std::string Path;
		std::string Name;
		bool IsDirectory = false;
	};

} // namespace Index
