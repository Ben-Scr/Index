#include "pch.hpp"
#include "Inspector/PropertyValue.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Axiom {

	namespace {
		// All conversions go through std::to_chars / std::from_chars so the
		// wire format is locale-immune (LC_NUMERIC won't turn "3.14" into
		// "3,14" or fail to parse it). The matching parser-side fix lives in
		// Json.cpp around lines 321-323.
		std::string FormatFloat(double v) {
			char buf[64];
			auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general);
			if (ec != std::errc{}) {
				return {};
			}
			return std::string(buf, ptr);
		}

		std::string FormatInt(int v) {
			char buf[32];
			auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
			if (ec != std::errc{}) {
				return {};
			}
			return std::string(buf, ptr);
		}

		std::vector<std::string> Split(const std::string& s, char sep) {
			std::vector<std::string> out;
			std::size_t start = 0;
			while (true) {
				const std::size_t at = s.find(sep, start);
				out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
				if (at == std::string::npos) break;
				start = at + 1;
			}
			return out;
		}

		float ToFloat(const std::string& s) {
			float result = 0.0f;
			const char* first = s.data();
			const char* last = first + s.size();
			auto [ptr, ec] = std::from_chars(first, last, result);
			(void)ptr;
			if (ec != std::errc{}) {
				return 0.0f;
			}
			return result;
		}

		int ToInt(const std::string& s) {
			int result = 0;
			const char* first = s.data();
			const char* last = first + s.size();
			auto [ptr, ec] = std::from_chars(first, last, result);
			(void)ptr;
			if (ec != std::errc{}) {
				return 0;
			}
			return result;
		}

		int64_t ToInt64(const std::string& s) {
			int64_t result = 0;
			const char* first = s.data();
			const char* last = first + s.size();
			auto [ptr, ec] = std::from_chars(first, last, result);
			(void)ptr;
			if (ec != std::errc{}) {
				return 0;
			}
			return result;
		}

		uint64_t ToUInt64(const std::string& s) {
			uint64_t result = 0;
			const char* first = s.data();
			const char* last = first + s.size();
			auto [ptr, ec] = std::from_chars(first, last, result);
			(void)ptr;
			if (ec != std::errc{}) {
				return 0;
			}
			return result;
		}

		double ToDouble(const std::string& s) {
			double result = 0.0;
			const char* first = s.data();
			const char* last = first + s.size();
			auto [ptr, ec] = std::from_chars(first, last, result);
			(void)ptr;
			if (ec != std::errc{}) {
				return 0.0;
			}
			return result;
		}
	}

	std::string PropertyValue::ToString() const {
		switch (Type) {
		case PropertyType::None:
			return {};
		case PropertyType::Bool:
			return BoolValue ? "true" : "false";
		case PropertyType::Int8:
		case PropertyType::Int16:
		case PropertyType::Int32:
		case PropertyType::Int64:
		case PropertyType::Enum:
		case PropertyType::FlagEnum:
			return std::to_string(IntValue);
		case PropertyType::UInt8:
		case PropertyType::UInt16:
		case PropertyType::UInt32:
		case PropertyType::UInt64:
			return std::to_string(UIntValue);
		case PropertyType::EntityRef:
			// Entity refs can also hold prefab references (the C# Entity type
			// transparently handles both); StringValue == "prefab" tags this
			// case so ToString reconstructs the "prefab:<id>" format.
			if (StringValue == "prefab" && UIntValue != 0) return "prefab:" + std::to_string(UIntValue);
			return UIntValue != 0 ? std::to_string(UIntValue) : std::string();
		case PropertyType::Float:
		case PropertyType::Double:
			return FormatFloat(FloatValue);
		case PropertyType::String:
			return StringValue;
		case PropertyType::StringList: {
			// Wire format: items joined by '\n', with embedded literal
			// '\n' / '\\' inside an item escaped as `\n` / `\\`. This
			// keeps the round-trip reversible without needing a real
			// JSON encoder for the simple list-of-strings case.
			std::string out;
			for (size_t i = 0; i < StringListValue.size(); ++i) {
				if (i > 0) out.push_back('\n');
				for (char c : StringListValue[i]) {
					if (c == '\\') out.append("\\\\");
					else if (c == '\n') out.append("\\n");
					else out.push_back(c);
				}
			}
			return out;
		}
		case PropertyType::List:
			// Native-only path today — descriptors built via
			// Properties::MakeList own the round-trip through their Get/Set
			// lambdas (no string serialization needed in-process). The C#
			// side hasn't shipped list-of-T support yet, so there's nothing
			// for ToString to encode here. Return empty rather than asserting
			// so any accidental call path keeps the round-trip lossless-empty.
			return {};
		case PropertyType::Vec2: {
			return FormatFloat(FloatVec[0]) + "," + FormatFloat(FloatVec[1]);
		}
		case PropertyType::Vec3: {
			return FormatFloat(FloatVec[0]) + "," + FormatFloat(FloatVec[1]) + "," +
				FormatFloat(FloatVec[2]);
		}
		case PropertyType::Vec4:
		case PropertyType::Color: {
			return FormatFloat(FloatVec[0]) + "," + FormatFloat(FloatVec[1]) + "," +
				FormatFloat(FloatVec[2]) + "," + FormatFloat(FloatVec[3]);
		}
		case PropertyType::IntVec2: {
			return FormatInt(IntVec[0]) + "," + FormatInt(IntVec[1]);
		}
		case PropertyType::IntVec3: {
			return FormatInt(IntVec[0]) + "," + FormatInt(IntVec[1]) + "," +
				FormatInt(IntVec[2]);
		}
		case PropertyType::IntVec4: {
			return FormatInt(IntVec[0]) + "," + FormatInt(IntVec[1]) + "," +
				FormatInt(IntVec[2]) + "," + FormatInt(IntVec[3]);
		}
		case PropertyType::TextureRef:
		case PropertyType::AudioRef:
		case PropertyType::AssetRef:
		case PropertyType::SceneRef:
		case PropertyType::FontRef:
			return UIntValue != 0 ? std::to_string(UIntValue) : std::string();
		case PropertyType::PrefabRef:
			return UIntValue != 0 ? "prefab:" + std::to_string(UIntValue) : std::string();
		case PropertyType::ComponentRef:
			if (UIntValue == 0) return std::string();
			return std::to_string(UIntValue) + ":" + StringValue;
		}
		return {};
	}

	PropertyValue PropertyValue::FromString(PropertyType type, const std::string& text) {
		PropertyValue v;
		v.Type = type;

		switch (type) {
		case PropertyType::None:
			break;
		case PropertyType::Bool:
			v.BoolValue = (text == "true" || text == "True" || text == "1");
			break;
		case PropertyType::Int8:
		case PropertyType::Int16:
		case PropertyType::Int32:
		case PropertyType::Int64:
		case PropertyType::Enum:
		case PropertyType::FlagEnum:
			v.IntValue = ToInt64(text);
			// Clamp narrower signed types so an out-of-range string can't
			// poison the in-memory value with bits the field can't hold.
			switch (type) {
			case PropertyType::Int8:
				v.IntValue = std::clamp<int64_t>(v.IntValue, INT8_MIN, INT8_MAX);
				break;
			case PropertyType::Int16:
				v.IntValue = std::clamp<int64_t>(v.IntValue, INT16_MIN, INT16_MAX);
				break;
			case PropertyType::Int32:
				v.IntValue = std::clamp<int64_t>(v.IntValue, INT32_MIN, INT32_MAX);
				break;
			case PropertyType::Enum:
				// TODO: validate against EnumDescriptor::Options when the
				// descriptor is plumbed through to PropertyValue. For now
				// clamp to int32 range — the typical underlying type.
				v.IntValue = std::clamp<int64_t>(v.IntValue, INT32_MIN, INT32_MAX);
				break;
			case PropertyType::FlagEnum:
				// TODO: mask against the OR of EnumDescriptor::Options[].Value
				// when the descriptor is plumbed through to PropertyValue,
				// to reject undeclared bits. The drawer-side mask is the
				// authoritative gate today.
				v.IntValue = std::clamp<int64_t>(v.IntValue, INT32_MIN, INT32_MAX);
				break;
			default:
				break;
			}
			break;
		case PropertyType::UInt8:
		case PropertyType::UInt16:
		case PropertyType::UInt32:
		case PropertyType::UInt64:
			v.UIntValue = ToUInt64(text);
			switch (type) {
			case PropertyType::UInt8:
				v.UIntValue = std::clamp<uint64_t>(v.UIntValue, 0u, UINT8_MAX);
				break;
			case PropertyType::UInt16:
				v.UIntValue = std::clamp<uint64_t>(v.UIntValue, 0u, UINT16_MAX);
				break;
			case PropertyType::UInt32:
				v.UIntValue = std::clamp<uint64_t>(v.UIntValue, 0u, UINT32_MAX);
				break;
			default:
				break;
			}
			break;
		case PropertyType::EntityRef: {
			static constexpr std::string_view prefabPrefix = "prefab:";
			if (text.rfind(prefabPrefix, 0) == 0) {
				v.StringValue = "prefab";
				v.UIntValue = ToUInt64(text.substr(prefabPrefix.size()));
			}
			else {
				v.StringValue.clear();
				v.UIntValue = text.empty() ? 0 : ToUInt64(text);
			}
			break;
		}
		case PropertyType::Float:
		case PropertyType::Double:
			v.FloatValue = ToDouble(text);
			break;
		case PropertyType::String:
			v.StringValue = text;
			break;
		case PropertyType::StringList: {
			// Inverse of ToString's wire format. Empty input → empty
			// list (NOT a list with one empty string), so a freshly-
			// initialised StringList round-trips as no entries.
			v.StringListValue.clear();
			if (text.empty()) break;
			std::string current;
			for (size_t i = 0; i < text.size(); ++i) {
				char c = text[i];
				if (c == '\\' && i + 1 < text.size()) {
					char next = text[i + 1];
					if (next == 'n')      { current.push_back('\n'); ++i; continue; }
					if (next == '\\')     { current.push_back('\\'); ++i; continue; }
				}
				if (c == '\n') {
					v.StringListValue.push_back(std::move(current));
					current.clear();
					continue;
				}
				current.push_back(c);
			}
			v.StringListValue.push_back(std::move(current));
			break;
		}
		case PropertyType::Vec2:
		case PropertyType::Vec3:
		case PropertyType::Vec4:
		case PropertyType::Color: {
			const auto parts = Split(text, ',');
			const std::size_t n = (type == PropertyType::Vec2) ? 2
				: (type == PropertyType::Vec3) ? 3
				: 4;
			for (std::size_t i = 0; i < n && i < parts.size(); ++i) {
				v.FloatVec[i] = ToFloat(parts[i]);
			}
			break;
		}
		case PropertyType::IntVec2:
		case PropertyType::IntVec3:
		case PropertyType::IntVec4: {
			const auto parts = Split(text, ',');
			const std::size_t n = (type == PropertyType::IntVec2) ? 2
				: (type == PropertyType::IntVec3) ? 3
				: 4;
			for (std::size_t i = 0; i < n && i < parts.size(); ++i) {
				v.IntVec[i] = ToInt(parts[i]);
			}
			break;
		}
		case PropertyType::TextureRef:
		case PropertyType::AudioRef:
		case PropertyType::AssetRef:
		case PropertyType::SceneRef:
		case PropertyType::FontRef:
			v.UIntValue = text.empty() ? 0 : ToUInt64(text);
			break;
		case PropertyType::PrefabRef: {
			static constexpr std::string_view prefix = "prefab:";
			if (text.rfind(prefix, 0) == 0) {
				v.UIntValue = ToUInt64(text.substr(prefix.size()));
			}
			else if (!text.empty()) {
				v.UIntValue = ToUInt64(text);
			}
			break;
		}
		case PropertyType::ComponentRef: {
			const std::size_t sep = text.find(':');
			if (sep == std::string::npos) {
				v.UIntValue = 0;
				v.StringValue.clear();
			}
			else {
				v.UIntValue = ToUInt64(text.substr(0, sep));
				v.StringValue = text.substr(sep + 1);
			}
			break;
		}
		case PropertyType::List:
			// Same rationale as ToString — native-only, no string round-trip
			// needed for in-process descriptors. Falls through with an empty
			// ListValue so a caller that ignores the type tag still gets a
			// well-formed PropertyValue.
			break;
		}
		return v;
	}

} // namespace Axiom
