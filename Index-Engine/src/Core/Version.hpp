#pragma once

#define IDX_VERSION "2026.1.0"

//
// Build Configuration
//
#if defined(IDX_DEBUG)
#define IDX_BUILD_CONFIG_NAME "Debug"
#elif defined(IDX_RELEASE)
#define IDX_BUILD_CONFIG_NAME "Release"
#elif defined(IDX_DIST)
#define IDX_BUILD_CONFIG_NAME "Dist"
#else
#error Undefined configuration?
#endif

//
// Build Platform
//
#if defined(IDX_PLATFORM_WINDOWS)
#define IDX_BUILD_PLATFORM_NAME "Windows x64"
#elif defined(IDX_PLATFORM_LINUX)
#define IDX_BUILD_PLATFORM_NAME "Linux x64"
#else
#error Unsupported Platform!
#endif

#define IDX_VERSION_LONG "Index " IDX_VERSION " (" IDX_BUILD_PLATFORM_NAME " " IDX_BUILD_CONFIG_NAME ")"

#include <cstdint>
#include <string_view>

namespace Index::Version {

	// Returns actual >= required; missing segments = 0, trailing suffix (e.g. "-rc1") ignored.
	inline bool IsAtLeast(std::string_view actual, std::string_view required) {
		auto nextSegment = [](std::string_view& s) -> std::int64_t {
			std::int64_t value = 0;
			bool any = false;
			while (!s.empty()) {
				char c = s.front();
				if (c < '0' || c > '9') break;
				value = value * 10 + (c - '0');
				any = true;
				s.remove_prefix(1);
			}
			// Consume one dot separator if present; anything else (suffix like
			// '-rc1', trailing garbage) terminates parsing.
			if (!s.empty() && s.front() == '.') {
				s.remove_prefix(1);
			}
			else if (!s.empty()) {
				s = {};
			}
			return any ? value : 0;
		};

		while (!actual.empty() || !required.empty()) {
			std::int64_t a = nextSegment(actual);
			std::int64_t r = nextSegment(required);
			if (a < r) return false;
			if (a > r) return true;
		}
		return true;
	}

} // namespace Index::Version