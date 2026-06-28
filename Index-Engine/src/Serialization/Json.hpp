#pragma once

#include "Core/Export.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Index::Json {

	class INDEX_API Value {
	public:
		enum class Type {
			Null,
			Bool,
			Number,
			String,
			Object,
			Array
		};

		using Object = std::vector<std::pair<std::string, Value>>;
		using Array = std::vector<Value>;

		// Int64/UInt64 kinds preserve exact integer values > 2^53 (UUIDs); double loses precision for those.
		enum class NumberKind {
			Double,
			Int64,
			UInt64
		};

		Value() = default;
		Value(std::nullptr_t);
		Value(bool value);
		Value(double value);
		Value(int value);
		Value(int64_t value);
		Value(uint64_t value);
		Value(const char* value);
		Value(std::string value);

		static Value MakeObject();
		static Value MakeArray();

		Type GetType() const { return m_Type; }
		NumberKind GetNumberKind() const { return m_NumberKind; }
		bool IsNull() const { return m_Type == Type::Null; }
		bool IsBool() const { return m_Type == Type::Bool; }
		bool IsNumber() const { return m_Type == Type::Number; }
		bool IsString() const { return m_Type == Type::String; }
		bool IsObject() const { return m_Type == Type::Object; }
		bool IsArray() const { return m_Type == Type::Array; }

		bool AsBoolOr(bool fallback) const;
		double AsDoubleOr(double fallback) const;
		int AsIntOr(int fallback) const;
		int64_t AsInt64Or(int64_t fallback) const;
		uint64_t AsUInt64Or(uint64_t fallback) const;
		std::string AsStringOr(std::string fallback = {}) const;

		Object& GetObject();
		const Object& GetObject() const;
		Array& GetArray();
		const Array& GetArray() const;

		Object& EnsureObject();
		Array& EnsureArray();

		Value* FindMember(std::string_view key);
		const Value* FindMember(std::string_view key) const;

		Value& AddMember(std::string key, Value value);

		// Append a member WITHOUT scanning for an existing key. ONLY safe when the caller
		// guarantees the key is unique (e.g. decoding a freshly-serialized object, whose
		// members were already de-duplicated at write time). Skips AddMember's O(n) dup scan,
		// turning a bulk object decode from O(n^2) into O(n).
		Value& AddMemberUnchecked(std::string key, Value value);
		// Pre-size the backing member vector to avoid reallocation during a known-size decode.
		void ReserveMembers(std::size_t count);

		Value& Append(Value value);

		// Structural equality (Int64(5) == UInt64(5) == Double(5.0)).
		INDEX_API friend bool operator==(const Value& a, const Value& b);
		friend bool operator!=(const Value& a, const Value& b) { return !(a == b); }

	private:
		void SetType(Type type);

	private:
		Type m_Type = Type::Null;
		NumberKind m_NumberKind = NumberKind::Double;
		bool m_Bool = false;
		double m_Number = 0.0;
		int64_t m_Int64 = 0;
		uint64_t m_UInt64 = 0;
		std::string m_String;
		Object m_Object;
		Array m_Array;
	};

	INDEX_API bool TryParse(std::string_view text, Value& outValue, std::string* outError = nullptr);
	INDEX_API Value Parse(std::string_view text, std::string* outError = nullptr);
	INDEX_API std::string EscapeString(std::string_view value);
	INDEX_API std::string Stringify(const Value& value, bool pretty = false, int indentSize = 2);

}
