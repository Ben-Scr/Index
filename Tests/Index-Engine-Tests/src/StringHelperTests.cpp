// Tests for Index::StringHelper — the small, fully-inline string utility
// collection (wrap, case conversion, trim, replace, starts/ends-with, byte-size
// formatting, substring removal). Every method here is header-inline.

#include <doctest/doctest.h>

#include "Core/Export.hpp"      // INDEX_API (StringHelper.hpp uses it but doesn't include this)
#include "Utils/StringHelper.hpp"

using namespace Index;

TEST_CASE("StringHelper::WrapWith — single char, distinct chars, and string marks") {
	CHECK(StringHelper::WrapWith("abc", '"') == "\"abc\"");
	CHECK(StringHelper::WrapWith("abc", '[', ']') == "[abc]");
	CHECK(StringHelper::WrapWith(std::string_view("abc"), std::string_view("**")) == "**abc**");
	CHECK(StringHelper::WrapWith(std::string_view("abc"), std::string_view("<<"), std::string_view(">>")) == "<<abc>>");

	// Empty payload still gets both marks.
	CHECK(StringHelper::WrapWith("", '|') == "||");
	CHECK(StringHelper::WrapWith(std::string_view(""), std::string_view("()")) == "()()");
}

TEST_CASE("StringHelper::ToString — concatenates streamed arguments") {
	CHECK(StringHelper::ToString("x=", 42) == "x=42");
	CHECK(StringHelper::ToString(1, '-', 2) == "1-2");
	CHECK(StringHelper::ToString() == "");          // no args -> empty
	CHECK(StringHelper::ToString("only") == "only");
}

TEST_CASE("StringHelper::ToLower / ToUpper — case conversion") {
	CHECK(StringHelper::ToLower("HeLLo World") == "hello world");
	CHECK(StringHelper::ToUpper("HeLLo World") == "HELLO WORLD");

	// Empty string is unchanged.
	CHECK(StringHelper::ToLower("") == "");
	CHECK(StringHelper::ToUpper("") == "");

	// Digits and punctuation pass through untouched.
	CHECK(StringHelper::ToLower("ABC-123") == "abc-123");
	CHECK(StringHelper::ToUpper("abc-123") == "ABC-123");
}

TEST_CASE("StringHelper::StartsWith — prefix matching with edge cases") {
	CHECK(StringHelper::StartsWith("filename.txt", "file"));
	CHECK(StringHelper::StartsWith("abc", "abc"));          // whole string
	CHECK(StringHelper::StartsWith("abc", ""));             // empty prefix always matches
	CHECK_FALSE(StringHelper::StartsWith("abc", "abcd"));   // prefix longer than string
	CHECK_FALSE(StringHelper::StartsWith("abc", "b"));      // not at the start
	CHECK_FALSE(StringHelper::StartsWith("", "a"));         // empty string, non-empty prefix
}

TEST_CASE("StringHelper::EndsWith — suffix matching with edge cases") {
	CHECK(StringHelper::EndsWith("filename.txt", ".txt"));
	CHECK(StringHelper::EndsWith("abc", "abc"));            // whole string
	CHECK(StringHelper::EndsWith("abc", ""));              // empty suffix always matches
	CHECK_FALSE(StringHelper::EndsWith("abc", "dabc"));     // suffix longer than string
	CHECK_FALSE(StringHelper::EndsWith("abc", "b"));        // not at the end
	CHECK_FALSE(StringHelper::EndsWith("", "a"));           // empty string, non-empty suffix
}

TEST_CASE("StringHelper::Replace — substitution, all-occurrences, and no-match") {
	CHECK(StringHelper::Replace("a.b.c", ".", "/") == "a/b/c");
	CHECK(StringHelper::Replace("aaa", "a", "bb") == "bbbbbb");   // every occurrence replaced
	CHECK(StringHelper::Replace("hello", "xyz", "Q") == "hello"); // no match -> unchanged

	// Empty 'from' returns the input verbatim (avoids an infinite loop).
	CHECK(StringHelper::Replace("hello", "", "X") == "hello");

	// Replacement text containing the search text does not re-trigger (pos advances by to.length()).
	CHECK(StringHelper::Replace("cat", "cat", "category") == "category");

	// Replacing with empty string deletes matches.
	CHECK(StringHelper::Replace("a-b-c", "-", "") == "abc");
}

TEST_CASE("StringHelper::Trim — strips leading/trailing whitespace") {
	CHECK(StringHelper::Trim("  hello  ") == "hello");
	CHECK(StringHelper::Trim("\t\n hi \r\n") == "hi");
	CHECK(StringHelper::Trim("no-edges") == "no-edges");

	// Interior whitespace is preserved.
	CHECK(StringHelper::Trim("  a b  ") == "a b");

	// All-whitespace and empty inputs collapse to empty.
	CHECK(StringHelper::Trim("   ") == "");
	CHECK(StringHelper::Trim("") == "");
}

TEST_CASE("StringHelper::Remove — erases a substring range") {
	CHECK(StringHelper::Remove("abcdef", 2, 3) == "abf");      // remove "cde"
	CHECK(StringHelper::Remove("abcdef", 0, 2) == "cdef");     // remove from start
	CHECK(StringHelper::Remove("abcdef", 4, 10) == "abcd");    // count clamped to remaining length

	// No-op cases: count == 0, or start at/past the end.
	CHECK(StringHelper::Remove("abc", 1, 0) == "abc");
	CHECK(StringHelper::Remove("abc", 3, 1) == "abc");
	CHECK(StringHelper::Remove("abc", 99, 1) == "abc");
	CHECK(StringHelper::Remove("", 0, 1) == "");
}

TEST_CASE("StringHelper::ToIEC — binary (1024-based) byte formatting") {
	CHECK(StringHelper::ToIEC(0) == "0 B");
	CHECK(StringHelper::ToIEC(512) == "512 B");
	CHECK(StringHelper::ToIEC(1023) == "1023 B");
	CHECK(StringHelper::ToIEC(1024) == "1.00 KiB");
	CHECK(StringHelper::ToIEC(1536) == "1.50 KiB");                 // 1.5 * 1024
	CHECK(StringHelper::ToIEC(1024 * 1024) == "1.00 MiB");
	CHECK(StringHelper::ToIEC(static_cast<std::size_t>(1024) * 1024 * 1024) == "1.00 GiB");
}

TEST_CASE("StringHelper::ToSI — decimal (1000-based) byte formatting") {
	CHECK(StringHelper::ToSI(0) == "0 B");
	CHECK(StringHelper::ToSI(999) == "999 B");
	CHECK(StringHelper::ToSI(1000) == "1.00 kB");
	CHECK(StringHelper::ToSI(1500) == "1.50 kB");
	CHECK(StringHelper::ToSI(1000000) == "1.00 MB");
	CHECK(StringHelper::ToSI(1000000000) == "1.00 GB");
}
