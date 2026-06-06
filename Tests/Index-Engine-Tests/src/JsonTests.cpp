#include <doctest/doctest.h>

#include "Serialization/Json.hpp"

#include <cstdint>
#include <string>

using namespace Index;

// Scalar constructors set the right Type and the Is* predicates agree with it.
TEST_CASE("Json::Value scalar constructors report the matching type") {
	Json::Value nullValue;
	CHECK(nullValue.IsNull());
	CHECK(nullValue.GetType() == Json::Value::Type::Null);

	Json::Value explicitNull(nullptr);
	CHECK(explicitNull.IsNull());

	Json::Value boolValue(true);
	CHECK(boolValue.IsBool());
	CHECK_FALSE(boolValue.IsNumber());
	CHECK(boolValue.AsBoolOr(false) == true);

	Json::Value intValue(42);
	CHECK(intValue.IsNumber());
	CHECK(intValue.AsIntOr(0) == 42);

	Json::Value doubleValue(3.5);
	CHECK(doubleValue.IsNumber());
	CHECK(doubleValue.AsDoubleOr(0.0) == doctest::Approx(3.5));

	Json::Value stringValue(std::string("hello"));
	CHECK(stringValue.IsString());
	CHECK(stringValue.AsStringOr() == "hello");

	Json::Value cstrValue("world");
	CHECK(cstrValue.IsString());
	CHECK(cstrValue.AsStringOr("fallback") == "world");
}

// MakeObject/MakeArray produce the container kinds, and the As*Or accessors
// fall back when the stored type doesn't match the requested scalar.
TEST_CASE("Json::Value containers and typed accessors fall back on type mismatch") {
	Json::Value obj = Json::Value::MakeObject();
	CHECK(obj.IsObject());
	CHECK_FALSE(obj.IsArray());

	Json::Value arr = Json::Value::MakeArray();
	CHECK(arr.IsArray());
	CHECK_FALSE(arr.IsObject());

	// A string isn't a number/bool, so the typed accessors return the fallback.
	Json::Value text("not a number");
	CHECK(text.AsIntOr(7) == 7);
	CHECK(text.AsDoubleOr(1.25) == doctest::Approx(1.25));
	CHECK(text.AsBoolOr(true) == true);
	CHECK(text.AsUInt64Or(99ull) == 99ull);

	// A number isn't a string -> string fallback is returned verbatim.
	Json::Value number(5);
	CHECK(number.AsStringOr("missing") == "missing");
}

// UInt64/Int64 constructors preserve exact integers beyond double's 2^53 range.
TEST_CASE("Json::Value preserves wide integer values exactly") {
	const uint64_t big = 0xFFFFFFFFFFFFFFFFull; // would lose precision as double
	Json::Value u(big);
	CHECK(u.IsNumber());
	CHECK(u.GetNumberKind() == Json::Value::NumberKind::UInt64);
	CHECK(u.AsUInt64Or(0) == big);

	const int64_t signedBig = -9000000000000000000ll;
	Json::Value i(signedBig);
	CHECK(i.IsNumber());
	CHECK(i.GetNumberKind() == Json::Value::NumberKind::Int64);
	CHECK(i.AsInt64Or(0) == signedBig);
}

// AddMember inserts a keyed value; FindMember returns a pointer to it, or null
// for a key that is absent.
TEST_CASE("Json::Value AddMember and FindMember round-trip keyed members") {
	Json::Value obj = Json::Value::MakeObject();
	obj.AddMember("name", Json::Value("Index"));
	obj.AddMember("count", Json::Value(3));

	const Json::Value* name = obj.FindMember("name");
	REQUIRE(name != nullptr);
	CHECK(name->IsString());
	CHECK(name->AsStringOr() == "Index");

	const Json::Value* count = obj.FindMember("count");
	REQUIRE(count != nullptr);
	CHECK(count->AsIntOr(0) == 3);

	CHECK(obj.FindMember("missing") == nullptr);

	// AddMember returns a reference to the inserted value.
	Json::Value& added = obj.AddMember("flag", Json::Value(true));
	CHECK(added.AsBoolOr(false) == true);
}

// Append grows the array and the elements keep their values and order.
TEST_CASE("Json::Value Append builds an ordered array") {
	Json::Value arr = Json::Value::MakeArray();
	arr.Append(Json::Value(1));
	arr.Append(Json::Value("two"));
	arr.Append(Json::Value(true));

	REQUIRE(arr.IsArray());
	const Json::Value::Array& elems = arr.GetArray();
	REQUIRE(elems.size() == 3);
	CHECK(elems[0].AsIntOr(0) == 1);
	CHECK(elems[1].AsStringOr() == "two");
	CHECK(elems[2].AsBoolOr(false) == true);
}

// Parse -> Stringify -> Parse must preserve a nested document covering every
// scalar kind plus objects and arrays.
TEST_CASE("Json round-trip preserves nested objects, arrays, and scalars") {
	const std::string source =
		R"({"name":"index","count":7,"ratio":2.5,"enabled":true,"empty":null,)"
		R"("tags":["a","b","c"],"nested":{"x":1,"y":2}})";

	std::string firstError;
	Json::Value first = Json::Parse(source, &firstError);
	REQUIRE(first.IsObject());

	const std::string serialized = Json::Stringify(first);

	std::string secondError;
	Json::Value second = Json::Parse(serialized, &secondError);
	REQUIRE(second.IsObject());

	CHECK(second.FindMember("name")->AsStringOr() == "index");
	CHECK(second.FindMember("count")->AsIntOr(0) == 7);
	CHECK(second.FindMember("ratio")->AsDoubleOr(0.0) == doctest::Approx(2.5));
	CHECK(second.FindMember("enabled")->AsBoolOr(false) == true);

	const Json::Value* empty = second.FindMember("empty");
	REQUIRE(empty != nullptr);
	CHECK(empty->IsNull());

	const Json::Value* tags = second.FindMember("tags");
	REQUIRE(tags != nullptr);
	REQUIRE(tags->IsArray());
	REQUIRE(tags->GetArray().size() == 3);
	CHECK(tags->GetArray()[0].AsStringOr() == "a");
	CHECK(tags->GetArray()[2].AsStringOr() == "c");

	const Json::Value* nested = second.FindMember("nested");
	REQUIRE(nested != nullptr);
	REQUIRE(nested->IsObject());
	CHECK(nested->FindMember("x")->AsIntOr(0) == 1);
	CHECK(nested->FindMember("y")->AsIntOr(0) == 2);
}

// A string with quote, backslash, and newline must survive Stringify -> Parse
// (these characters require JSON escaping).
TEST_CASE("Json round-trip preserves strings needing escaping") {
	const std::string tricky = "quote:\" backslash:\\ newline:\n end";

	Json::Value obj = Json::Value::MakeObject();
	obj.AddMember("text", Json::Value(tricky));

	const std::string serialized = Json::Stringify(obj);
	// The serialized form must escape, not embed, the raw control character.
	CHECK(serialized.find('\n') == std::string::npos);

	Json::Value parsed = Json::Parse(serialized);
	REQUIRE(parsed.IsObject());
	const Json::Value* text = parsed.FindMember("text");
	REQUIRE(text != nullptr);
	CHECK(text->AsStringOr() == tricky);

	// EscapeString emits an escaped backslash sequence for a backslash input.
	const std::string escaped = Json::EscapeString("a\\b");
	CHECK(escaped.find("\\\\") != std::string::npos);
}

// Malformed input must yield a value that is not an object, matching the guard
// in EditorPreferences ( if (!root.IsObject()) ... bail ).
TEST_CASE("Json::Parse on malformed input returns a non-object value") {
	std::string error;
	Json::Value bad = Json::Parse("{ this is not valid json", &error);
	CHECK_FALSE(bad.IsObject());

	Json::Value empty = Json::Parse("", nullptr);
	CHECK_FALSE(empty.IsObject());

	Json::Value garbage = Json::Parse("@@@", nullptr);
	CHECK_FALSE(garbage.IsObject());

	// TryParse signals failure via its bool return on the same malformed text.
	Json::Value out;
	CHECK_FALSE(Json::TryParse("{ broken", out));

	// A well-formed scalar parses successfully but still isn't an object.
	Json::Value scalar;
	CHECK(Json::TryParse("123", scalar));
	CHECK(scalar.IsNumber());
	CHECK_FALSE(scalar.IsObject());
}
