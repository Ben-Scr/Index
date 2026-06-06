// Tests for Index::Path — only the pure, deterministic surface is exercised.
//
// Path.hpp's only platform-stable, inline (always-linkable) function is the
// Combine(...) variadic template, which joins fragments with std::filesystem's
// '/=' operator and returns make_preferred().string(). The other members
// (GetSpecialFolderPath, ExecutableDir, ResolveIndexAssets,
// ToProjectRelativeDisplay, Current) query the real filesystem / environment,
// so they are intentionally skipped here.
//
// Combine's output uses the platform's preferred separator ('\\' on Windows,
// '/' on Linux). To stay cross-platform, expectations are derived from the same
// std::filesystem semantics rather than hardcoding a separator.

#include <doctest/doctest.h>

#include "Serialization/Path.hpp"

#include <filesystem>
#include <string>

using namespace Index;

namespace {
	namespace fs = std::filesystem;

	// The separator Combine() will emit, as a single-char string.
	const std::string kSep(1, static_cast<char>(fs::path::preferred_separator));

	// Reference implementation mirroring Path::Combine for two/three fragments,
	// using the exact same std::filesystem operations the production code uses.
	std::string ExpectedCombine(const std::string& a, const std::string& b) {
		fs::path p;
		p /= fs::path(a);
		p /= fs::path(b);
		return p.make_preferred().string();
	}

	std::string ExpectedCombine(const std::string& a, const std::string& b, const std::string& c) {
		fs::path p;
		p /= fs::path(a);
		p /= fs::path(b);
		p /= fs::path(c);
		return p.make_preferred().string();
	}
} // namespace

TEST_CASE("Path::Combine joins fragments with the platform-preferred separator") {
	const std::string result = Path::Combine("foo", "bar");
	CHECK(result == ExpectedCombine("foo", "bar"));
	// The single embedded separator must be the OS-preferred one.
	CHECK(result == "foo" + kSep + "bar");
}

TEST_CASE("Path::Combine chains an arbitrary number of fragments") {
	const std::string result = Path::Combine("a", "b", "c");
	CHECK(result == ExpectedCombine("a", "b", "c"));
	CHECK(result == "a" + kSep + "b" + kSep + "c");
}

TEST_CASE("Path::Combine with a single fragment returns that fragment unchanged") {
	CHECK(Path::Combine("solo") == "solo");
	CHECK(Path::Combine("name.ext") == "name.ext");
}

TEST_CASE("Path::Combine normalizes forward-slash separators to the preferred separator") {
	// A forward-slash input fragment is parsed as a relative path with two
	// components, so make_preferred() rewrites the separator on Windows and
	// leaves it untouched on Linux.
	const std::string result = Path::Combine("foo/bar");
	CHECK(result == ExpectedCombine("foo", "bar"));
	CHECK(result == "foo" + kSep + "bar");
}

TEST_CASE("Path::Combine treats an absolute-looking second fragment as filesystem operator/= does") {
	// operator/= replaces the path when the right-hand side is absolute. Mirror
	// whatever std::filesystem decides so the test is identical on both OSes.
	const std::string lhsThenRooted = Path::Combine("foo", "/bar");
	fs::path expected;
	expected /= fs::path("foo");
	expected /= fs::path("/bar");
	CHECK(lhsThenRooted == expected.make_preferred().string());
}

TEST_CASE("Path::Combine preserves file extensions in the final fragment") {
	const std::string result = Path::Combine("dir", "asset.dataasset");
	CHECK(result == ExpectedCombine("dir", "asset.dataasset"));
	CHECK(result == "dir" + kSep + "asset.dataasset");
	// The trailing extension fragment survives intact.
	CHECK(fs::path(result).extension().string() == ".dataasset");
	CHECK(fs::path(result).filename().string() == "asset.dataasset");
	CHECK(fs::path(result).stem().string() == "asset");
}

TEST_CASE("Path::Combine is deterministic and repeatable for equal inputs") {
	CHECK(Path::Combine("x", "y", "z") == Path::Combine("x", "y", "z"));
	// Order matters: swapping fragments yields a different join.
	CHECK(Path::Combine("x", "y") != Path::Combine("y", "x"));
}

TEST_CASE("Path::Combine accepts std::string fragments as well as string literals") {
	const std::string a = "Textures";
	const std::string b = "icon.png";
	const std::string result = Path::Combine(a, b);
	CHECK(result == ExpectedCombine("Textures", "icon.png"));
	CHECK(result == "Textures" + kSep + "icon.png");
}

TEST_CASE("Path::Combine with an empty leading fragment yields the second fragment") {
	// p /= path("") is a no-op; appending "leaf" then gives just "leaf".
	CHECK(Path::Combine(std::string(""), std::string("leaf")) == ExpectedCombine("", "leaf"));
	CHECK(Path::Combine(std::string(""), std::string("leaf")) == "leaf");
}
