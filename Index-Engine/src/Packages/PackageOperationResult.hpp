#pragma once

#include <string>

namespace Index {

	struct PackageOperationResult {
		bool Success = false;
		std::string Message;
	};

} // namespace Index
