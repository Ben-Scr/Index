#pragma once

#include "Core/Export.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace Index::Hash {

	// Windows uses BCrypt; non-Windows returns false/"not implemented" — OpenSSL/libsodium deferred until the Linux launcher has a live downloader.
	INDEX_API bool Sha256OfFile(const std::filesystem::path& filePath,
		std::string& outHexLower,
		std::string& outError);

	// Returns `s` with ASCII letters lowered. Used to normalize user-supplied
	// expected-hash strings before equality compare.
	INDEX_API std::string ToLowerHex(std::string_view s);

} // namespace Index::Hash
