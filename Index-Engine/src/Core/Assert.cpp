#include "pch.hpp"

#include "Core/Assert.hpp"

#include <cstdlib>

namespace Index {

	void ReportAssertionFailure(const char* kind, const char* expression, const std::string_view message, const std::source_location location) {
		const std::string formatted = fmt::format(
			"{} failed: ({}) | {} @ {}:{} ({})",
			kind,
			expression,
			message,
			location.file_name(),
			location.line(),
			location.function_name());

		IDX_CORE_ERROR("{}", formatted);

#if defined(IDX_DEBUG) || defined(IDX_RELEASE)
		// Release is a developer build: fast-fail same as Debug. Only Dist (shipped) continues.
		IDX_DEBUG_BREAK;
		std::terminate();
#elif defined(IDX_DIST)
		// Shipped build: report and continue. The error log stream above is the
		// only feedback path for end users — don't terminate the process.
#else
		std::terminate();
#endif
	}

}