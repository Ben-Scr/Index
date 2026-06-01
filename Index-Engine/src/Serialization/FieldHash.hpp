#pragma once

#include <cstdint>
#include <string_view>

namespace Index::Serialization {

	// FNV-1a 32-bit; ABI-stable for embedding in serialized assets. 32 bits is sufficient because collisions are scoped per-component (the outer component header selects the Serialize() before field hashes are resolved).
	constexpr std::uint32_t FieldHash(std::string_view name) noexcept {
		constexpr std::uint32_t kOffsetBasis = 0x811c9dc5u;
		constexpr std::uint32_t kPrime = 0x01000193u;
		std::uint32_t h = kOffsetBasis;
		for (char c : name) {
			h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
			h *= kPrime;
		}
		return h;
	}

} // namespace Index::Serialization
