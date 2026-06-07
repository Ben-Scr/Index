#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Every field maps to exactly one PropertyType; drawer, serializer, and reference picker all dispatch on this enum.
// EntityRef is polymorphic: UIntValue holds the ID; StringValue == "prefab" tags prefab asset refs — callers reading UIntValue directly MUST also check StringValue.

namespace Index {

	// Single point of truth for the empty-reference label; change here to retheme all inspector sites.
	inline constexpr const char* k_NoneLabel = "(None)";

	enum class PropertyType : uint8_t {
		None = 0,

		// Booleans
		Bool,

		// Signed integers
		Int8,
		Int16,
		Int32,
		Int64,

		// Unsigned integers
		UInt8,
		UInt16,
		UInt32,
		UInt64,

		// Floating point
		Float,
		Double,

		// Strings
		String,
		StringList, // Legacy string-only list; kept so existing serializers don't change shape.
		List,       // Generic list of any primitive PropertyType; item type in PropertyMetadata::ListItemType.

		// Vectors
		Vec2,
		Vec3,
		Vec4,
		IntVec2,
		IntVec3,
		IntVec4,

		// Colour
		Color,

		// Enumerations
		Enum,
		FlagEnum,

		// References
		TextureRef,
		AudioRef,
		FontRef,
		AssetRef,     // Generic asset of unspecified kind (filtered by metadata)
		SceneRef,
		EntityRef,
		PrefabRef,
		ComponentRef,

		// Inline animation curve (C# Index.Graph). The keys ride in StringValue as the
		// shared ';'/',' codec; drawn with the curve editor, not a reference picker.
		Graph,
	};

	struct EnumOption {
		std::string Name;
		int64_t Value = 0;
	};

	// Shared metadata for an Enum / FlagEnum descriptor. Lives outside
	// PropertyMetadata so language-agnostic code can build it from either
	// magic_enum (C++) or System.Reflection (C#).
	struct EnumDescriptor {
		std::vector<EnumOption> Options;
		bool IsFlags = false;
	};

	constexpr std::string_view ToString(PropertyType type) {
		switch (type) {
		case PropertyType::None:         return "none";
		case PropertyType::Bool:         return "bool";
		case PropertyType::Int8:         return "sbyte";
		case PropertyType::Int16:        return "short";
		case PropertyType::Int32:        return "int";
		case PropertyType::Int64:        return "long";
		case PropertyType::UInt8:        return "byte";
		case PropertyType::UInt16:       return "ushort";
		case PropertyType::UInt32:       return "uint";
		case PropertyType::UInt64:       return "ulong";
		case PropertyType::Float:        return "float";
		case PropertyType::Double:       return "double";
		case PropertyType::String:       return "string";
		case PropertyType::StringList:   return "stringList";
		case PropertyType::List:         return "list";
		case PropertyType::Vec2:         return "vector2";
		case PropertyType::Vec3:         return "vector3";
		case PropertyType::Vec4:         return "vector4";
		case PropertyType::IntVec2:      return "vector2Int";
		case PropertyType::IntVec3:      return "vector3Int";
		case PropertyType::IntVec4:      return "vector4Int";
		case PropertyType::Color:        return "color";
		case PropertyType::Enum:         return "enum";
		case PropertyType::FlagEnum:     return "flagenum";
		case PropertyType::TextureRef:   return "texture";
		case PropertyType::AudioRef:     return "audio";
		case PropertyType::FontRef:      return "font";
		case PropertyType::AssetRef:     return "asset";
		case PropertyType::SceneRef:     return "scene";
		case PropertyType::EntityRef:    return "entity";
		case PropertyType::PrefabRef:    return "prefab";
		case PropertyType::ComponentRef: return "component";
		case PropertyType::Graph:        return "graph";
		}
		return "none";
	}

	// Inverse of ToString. ComponentRef JSON carries a ":TypeName" suffix the caller strips first; this matches only the bare "component" tag.
	constexpr PropertyType PropertyTypeFromString(std::string_view text) {
		if (text == "bool")        return PropertyType::Bool;
		if (text == "sbyte")       return PropertyType::Int8;
		if (text == "short")       return PropertyType::Int16;
		if (text == "int")         return PropertyType::Int32;
		if (text == "long")        return PropertyType::Int64;
		if (text == "byte")        return PropertyType::UInt8;
		if (text == "ushort")      return PropertyType::UInt16;
		if (text == "uint")        return PropertyType::UInt32;
		if (text == "ulong")       return PropertyType::UInt64;
		if (text == "float")       return PropertyType::Float;
		if (text == "double")      return PropertyType::Double;
		if (text == "string")      return PropertyType::String;
		if (text == "stringList")  return PropertyType::StringList;
		if (text == "list")        return PropertyType::List;
		if (text == "vector2")     return PropertyType::Vec2;
		if (text == "vector3")     return PropertyType::Vec3;
		if (text == "vector4")     return PropertyType::Vec4;
		if (text == "vector2Int")  return PropertyType::IntVec2;
		if (text == "vector3Int")  return PropertyType::IntVec3;
		if (text == "vector4Int")  return PropertyType::IntVec4;
		if (text == "color")       return PropertyType::Color;
		if (text == "enum")        return PropertyType::Enum;
		if (text == "flagenum")    return PropertyType::FlagEnum;
		if (text == "texture")     return PropertyType::TextureRef;
		if (text == "audio")       return PropertyType::AudioRef;
		if (text == "asset")       return PropertyType::AssetRef;
		if (text == "scene")       return PropertyType::SceneRef;
		if (text == "font")        return PropertyType::FontRef;
		if (text == "entity")      return PropertyType::EntityRef;
		if (text == "prefab")      return PropertyType::PrefabRef;
		if (text == "component")   return PropertyType::ComponentRef;
		if (text == "graph")       return PropertyType::Graph;
		return PropertyType::None;
	}

} // namespace Index
