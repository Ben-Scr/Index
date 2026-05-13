#pragma once
#include "Core/IndexErrorCode.hpp"
#include "Core/Export.hpp"

#include <exception>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

#include "Core/Log.hpp"

namespace Index {
	INDEX_API const char* ErrorCodeToString(IndexErrorCode code);

	class INDEX_API IndexException : public std::runtime_error {
	public:
		IndexException(IndexErrorCode code, std::string message, std::source_location location = std::source_location::current());

		IndexErrorCode Code() const noexcept { return m_Code; }
		const std::string& Message() const noexcept { return m_Message; }
		const std::source_location& Location() const noexcept { return m_Location; }

	private:
		static std::string BuildWhat(IndexErrorCode code, std::string_view message, const std::source_location& location);

		IndexErrorCode m_Code;
		std::string m_Message;
		std::source_location m_Location;
	};

	using IndexError = IndexException;

	[[noreturn]] INDEX_API void ThrowError(IndexErrorCode code, std::string_view message, std::source_location location = std::source_location::current());
	[[noreturn]] INDEX_API void RethrowWithContext(IndexErrorCode code, std::string message, std::source_location location = std::source_location::current());

} // namespace Index

#define IDX_THROW(code, msg) ::Index::ThrowError((code), (msg), std::source_location::current())

#define IDX_LOG_ERROR(code, msg) \
	do { \
		IDX_CORE_ERROR("[{}] {}", ::Index::ErrorCodeToString(code), (msg)); \
	} while (0)

#define INDEX_LOG_ERROR_IF(cond, code, msg) \
	do { \
		if (cond) { \
			IDX_LOG_ERROR((code), (msg)); \
		} \
	} while (0)

#define INDEX_TRY_CATCH_LOG(stmt) \
	do { \
		try { \
			stmt; \
		} catch (const std::exception& ex) { \
			IDX_CORE_ERROR("Exception: {}", ex.what()); \
		} catch (...) { \
			IDX_CORE_ERROR("Unknown exception"); \
		} \
	} while (0)
