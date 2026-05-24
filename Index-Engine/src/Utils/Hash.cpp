#include "pch.hpp"
#include "Utils/Hash.hpp"

#include <array>
#include <cctype>
#include <fstream>
#include <vector>

#ifdef IDX_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace Index::Hash {

	std::string ToLowerHex(std::string_view s) {
		std::string out;
		out.reserve(s.size());
		for (char c : s)
			out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		return out;
	}

#ifdef IDX_PLATFORM_WINDOWS

	bool Sha256OfFile(const std::filesystem::path& filePath,
		std::string& outHexLower,
		std::string& outError)
	{
		BCRYPT_ALG_HANDLE algHandle = nullptr;
		NTSTATUS status = BCryptOpenAlgorithmProvider(&algHandle, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
		if (status != 0) { outError = "BCryptOpenAlgorithmProvider failed"; return false; }

		struct AlgGuard { BCRYPT_ALG_HANDLE h; ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); } } algGuard{ algHandle };

		DWORD hashObjLen = 0;
		DWORD cbResult = 0;
		status = BCryptGetProperty(algHandle, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&hashObjLen), sizeof(hashObjLen), &cbResult, 0);
		if (status != 0) { outError = "BCryptGetProperty(OBJECT_LENGTH) failed"; return false; }

		std::vector<UCHAR> hashObj(hashObjLen);
		BCRYPT_HASH_HANDLE hashHandle = nullptr;
		status = BCryptCreateHash(algHandle, &hashHandle, hashObj.data(), hashObjLen, nullptr, 0, 0);
		if (status != 0) { outError = "BCryptCreateHash failed"; return false; }

		struct HashGuard { BCRYPT_HASH_HANDLE h; ~HashGuard() { if (h) BCryptDestroyHash(h); } } hashGuard{ hashHandle };

		std::ifstream in(filePath, std::ios::binary);
		if (!in.is_open()) { outError = "open failed"; return false; }

		std::array<char, 64 * 1024> buf{};
		while (in.good()) {
			in.read(buf.data(), buf.size());
			const std::streamsize n = in.gcount();
			if (n <= 0) break;
			status = BCryptHashData(hashHandle, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0);
			if (status != 0) { outError = "BCryptHashData failed"; return false; }
		}

		std::array<UCHAR, 32> digest{};
		status = BCryptFinishHash(hashHandle, digest.data(), static_cast<ULONG>(digest.size()), 0);
		if (status != 0) { outError = "BCryptFinishHash failed"; return false; }

		static const char* k_Hex = "0123456789abcdef";
		std::string hex;
		hex.reserve(digest.size() * 2);
		for (UCHAR b : digest) {
			hex.push_back(k_Hex[(b >> 4) & 0xF]);
			hex.push_back(k_Hex[b & 0xF]);
		}
		outHexLower = std::move(hex);
		return true;
	}

#else

	bool Sha256OfFile(const std::filesystem::path&, std::string& outHexLower, std::string& outError) {
		outHexLower.clear();
		outError = "SHA-256 verification not implemented on this platform";
		return false;
	}

#endif

} // namespace Index::Hash
