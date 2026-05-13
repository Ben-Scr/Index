#pragma once

#include "Core/Base.hpp"
#include "Core/Exceptions.hpp"
#include "Core/Log.hpp"

#include <source_location>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/fmt/fmt.h>

#ifdef IDX_PLATFORM_WINDOWS
#define IDX_DEBUG_BREAK __debugbreak()
#else
#include <csignal>
#define IDX_DEBUG_BREAK raise(SIGTRAP)
#endif

namespace Index {

	INDEX_API void ReportAssertionFailure(
		const char* kind,
		const char* expression,
		std::string_view message,
		std::source_location location = std::source_location::current());

	inline std::string BuildAssertMessage(const std::string_view message) {
		return std::string(message);
	}

	inline std::string BuildAssertMessage(const IndexErrorCode code, const std::string_view message) {
		return fmt::format("[{}] {}", ErrorCodeToString(code), message);
	}

	template <typename T>
	inline std::string BuildAssertMessage(T&& message) {
		return std::string(std::forward<T>(message));
	}

} // namespace Index

#ifdef IDX_DEBUG
#define IDX_ENABLE_ASSERTS
#endif

#define IDX_ENABLE_VERIFY

#define IDX_ASSERT(cond, ...) \
	do { \
		if (!(cond)) { \
			::Index::ReportAssertionFailure("ASSERT", #cond, ::Index::BuildAssertMessage(__VA_ARGS__), std::source_location::current()); \
		} \
	} while (0)

#define IDX_CORE_ASSERT(cond, ...) \
	do { \
		if (!(cond)) { \
			::Index::ReportAssertionFailure("CORE_ASSERT", #cond, ::Index::BuildAssertMessage(__VA_ARGS__), std::source_location::current()); \
		} \
	} while (0)

#ifdef IDX_ENABLE_VERIFY
#define IDX_VERIFY(cond, ...) \
	do { \
		if (!(cond)) { \
			::Index::ReportAssertionFailure("VERIFY", #cond, ::Index::BuildAssertMessage(__VA_ARGS__), std::source_location::current()); \
		} \
	} while (0)

#define IDX_CORE_VERIFY(cond, ...) \
	do { \
		if (!(cond)) { \
			::Index::ReportAssertionFailure("CORE_VERIFY", #cond, ::Index::BuildAssertMessage(__VA_ARGS__), std::source_location::current()); \
		} \
	} while (0)
#else
#define IDX_VERIFY(cond, ...) ((void)(cond))
#define IDX_CORE_VERIFY(cond, ...) ((void)(cond))
#endif