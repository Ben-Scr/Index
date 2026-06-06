#include <doctest/doctest.h>

#include "Core/UUID.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <string>

using namespace Index;

TEST_CASE("UUID round-trips a uint64_t value through construction and conversion") {
	const uint64_t value = 0x0123456789ABCDEFull;

	UUID id(value);

	// Implicit conversion back to uint64_t must yield the original bits.
	uint64_t roundTripped = id;
	CHECK(roundTripped == value);

	// The const conversion operator path must agree.
	const UUID constId(value);
	uint64_t constRoundTripped = constId;
	CHECK(constRoundTripped == value);
}

TEST_CASE("UUID preserves boundary uint64_t values") {
	const uint64_t zero = 0ull;
	const uint64_t max  = ~0ull;  // 0xFFFFFFFFFFFFFFFF

	UUID zeroId(zero);
	UUID maxId(max);

	CHECK(static_cast<uint64_t>(zeroId) == zero);
	CHECK(static_cast<uint64_t>(maxId)  == max);
}

TEST_CASE("UUID copy construction preserves the underlying value") {
	UUID original(0xDEADBEEFCAFEBABEull);
	UUID copy(original);

	CHECK(static_cast<uint64_t>(copy) == static_cast<uint64_t>(original));
	CHECK(static_cast<uint64_t>(copy) == 0xDEADBEEFCAFEBABEull);
}

TEST_CASE("Default-constructed UUIDs are distinct across a large batch") {
	// The default ctor draws from a random distribution; collisions among
	// 1000 64-bit draws are astronomically unlikely, so all-distinct is a
	// safe deterministic-enough assertion.
	constexpr int kCount = 1000;

	std::unordered_set<uint64_t> seen;
	seen.reserve(kCount);

	for (int i = 0; i < kCount; ++i) {
		UUID id;  // random
		seen.insert(static_cast<uint64_t>(id));
	}

	CHECK(seen.size() == static_cast<size_t>(kCount));
}

TEST_CASE("std::hash<UUID> returns the underlying value as the hash") {
	const uint64_t value = 0x00FF00FF00FF00FFull;
	UUID id(value);

	std::hash<UUID> hasher;
	CHECK(hasher(id) == static_cast<size_t>(value));

	// Equal values hash equally; this is the contract a hash map relies on.
	UUID same(value);
	CHECK(hasher(id) == hasher(same));
}

TEST_CASE("UUID works as an unordered_map key") {
	std::unordered_map<UUID, std::string> table;

	UUID a(1ull);
	UUID b(2ull);
	UUID c(3ull);

	table[a] = "one";
	table[b] = "two";
	table[c] = "three";

	REQUIRE(table.size() == 3);
	CHECK(table.at(a) == "one");
	CHECK(table.at(b) == "two");
	CHECK(table.at(c) == "three");

	// A freshly constructed key with the same value must hit the same bucket.
	UUID aAgain(1ull);
	CHECK(table.find(aAgain) != table.end());
	CHECK(table.at(aAgain) == "one");

	// Overwriting through an equal-valued key updates the existing entry.
	table[aAgain] = "uno";
	CHECK(table.size() == 3);
	CHECK(table.at(a) == "uno");
}

TEST_CASE("Default-constructed random UUIDs serve as unique map keys") {
	constexpr int kCount = 256;

	std::unordered_map<UUID, int> table;
	table.reserve(kCount);

	for (int i = 0; i < kCount; ++i) {
		UUID id;  // random
		table[id] = i;
	}

	// With overwhelming probability every random key was distinct, so the
	// map holds exactly kCount entries.
	CHECK(table.size() == static_cast<size_t>(kCount));
}
