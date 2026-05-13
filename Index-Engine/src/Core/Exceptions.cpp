#include "pch.hpp"

#include "Core/Exceptions.hpp"

#include <utility>
#include <spdlog/fmt/fmt.h>

namespace Index {

	const char* ErrorCodeToString(const IndexErrorCode code) {
		switch (code) {
		case IndexErrorCode::InvalidArgument: return "InvalidArgument";
		case IndexErrorCode::NotInitialized: return "NotInitialized";
		case IndexErrorCode::AlreadyInitialized: return "AlreadyInitialized";
		case IndexErrorCode::FileNotFound: return "FileNotFound";
		case IndexErrorCode::InvalidHandle: return "InvalidHandle";
		case IndexErrorCode::OutOfRange: return "OutOfRange";
		case IndexErrorCode::OutOfBounds: return "OutOfBounds";
		case IndexErrorCode::Overflow: return "Overflow";
		case IndexErrorCode::NullReference: return "NullReference";
		case IndexErrorCode::LoadFailed: return "LoadFailed";
		case IndexErrorCode::InvalidValue: return "InvalidValue";
		case IndexErrorCode::Undefined: return "Undefined";
		default: return "Undefined";
		}
	}

	IndexException::IndexException(const IndexErrorCode code, std::string message, const std::source_location location)
		: std::runtime_error(BuildWhat(code, message, location)),
		m_Code(code),
		m_Message(std::move(message)),
		m_Location(location) {
	}

	std::string IndexException::BuildWhat(const IndexErrorCode code, const std::string_view message, const std::source_location& location) {
		return fmt::format("IndexException[{}] {} @ {}:{} ({})",
			ErrorCodeToString(code),
			message,
			location.file_name(),
			location.line(),
			location.function_name());
	}

	[[noreturn]] void ThrowError(const IndexErrorCode code, const std::string_view message, const std::source_location location) {
		throw IndexException(code, std::string(message), location);
	}

	[[noreturn]] void RethrowWithContext(const IndexErrorCode code, std::string message, const std::source_location location) {
		try {
			throw;
		}
		catch (const std::exception& ex) {
			message += std::string(" | caused by: ") + ex.what();
			throw IndexException(code, std::move(message), location);
		}
		catch (...) {
			message += " | caused by: <non-std exception>";
			throw IndexException(code, std::move(message), location);
		}
	}

} // namespace Index