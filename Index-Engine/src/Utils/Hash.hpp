#pragma once

#include "Core/Export.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace Index::Hash {

	// Computes SHA-256 over `filePath` and writes the digest as a 64-char
	// lowercase hex string to `outHexLower`. Returns true on success; on
	// failure returns false with a human-readable reason in `outError`.
	//
	// Windows uses BCrypt. On other platforms this currently returns false
	// with "not implemented" — the Linux launcher has no live downloader
	// today, so adding OpenSSL/libsodium is deferred until that path exists.
	INDEX_API bool Sha256OfFile(const std::filesystem::path& filePath,
		std::string& outHexLower,
		std::string& outError);

	// Returns `s` with ASCII letters lowered. Used to normalize user-supplied
	// expected-hash strings before equality compare.
	INDEX_API std::string ToLowerHex(std::string_view s);

} // namespace Index::Hash
