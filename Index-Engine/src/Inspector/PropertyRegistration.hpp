#pragma once

#include "Collections/Color.hpp"
#include "Inspector/PropertyDescriptor.hpp"
#include "Inspector/PropertyType.hpp"
#include "Inspector/PropertyValue.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <magic_enum/magic_enum.hpp>

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Deduces PropertyType from C++ field types and builds PropertyDescriptors for native component registration.

namespace Index::Properties {

	namespace detail {

		template <typename T>
		constexpr PropertyType DeducePropertyType() {
			if constexpr (std::is_same_v<T, bool>) return PropertyType::Bool;
			else if constexpr (std::is_enum_v<T>) return PropertyType::Enum;
			else if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, signed char>) return PropertyType::Int8;
			else if constexpr (std::is_same_v<T, int16_t> || std::is_same_v<T, short>) return PropertyType::Int16;
			else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, int>) return PropertyType::Int32;
			else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, long long>) return PropertyType::Int64;
			else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, unsigned char>) return PropertyType::UInt8;
			else if constexpr (std::is_same_v<T, uint16_t> || std::is_same_v<T, unsigned short>) return PropertyType::UInt16;
			else if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, unsigned int>) return PropertyType::UInt32;
			else if constexpr (std::is_same_v<T, uint64_t> || std::is_same_v<T, unsigned long long>) return PropertyType::UInt64;
			// `long`/`unsigned long` are distinct types from int/long long and their
			// width is platform-dependent (LLP64=32-bit on Windows, LP64=64-bit on
			// Linux) — map by sizeof so they don't silently deduce to None.
			else if constexpr (std::is_same_v<T, long>) return sizeof(long) == 8 ? PropertyType::Int64 : PropertyType::Int32;
			else if constexpr (std::is_same_v<T, unsigned long>) return sizeof(unsigned long) == 8 ? PropertyType::UInt64 : PropertyType::UInt32;
			// plain `char` is a third distinct type from signed/unsigned char.
			else if constexpr (std::is_same_v<T, char>) return std::is_signed_v<char> ? PropertyType::Int8 : PropertyType::UInt8;
			else if constexpr (std::is_same_v<T, float>) return PropertyType::Float;
			else if constexpr (std::is_same_v<T, double>) return PropertyType::Double;
			else if constexpr (std::is_same_v<T, std::string>) return PropertyType::String;
			else if constexpr (std::is_same_v<T, glm::vec2>) return PropertyType::Vec2;
			else if constexpr (std::is_same_v<T, glm::vec3>) return PropertyType::Vec3;
			else if constexpr (std::is_same_v<T, glm::vec4>) return PropertyType::Vec4;
			else if constexpr (std::is_same_v<T, glm::ivec2>) return PropertyType::IntVec2;
			else if constexpr (std::is_same_v<T, glm::ivec3>) return PropertyType::IntVec3;
			else if constexpr (std::is_same_v<T, glm::ivec4>) return PropertyType::IntVec4;
			else if constexpr (std::is_same_v<T, Color>) return PropertyType::Color;
			else return PropertyType::None;
		}

		template <typename T>
		PropertyValue Box(const T& value) {
			PropertyValue v;
			v.Type = DeducePropertyType<T>();
			if constexpr (std::is_same_v<T, bool>) v.BoolValue = value;
			else if constexpr (std::is_enum_v<T>) v.IntValue = static_cast<int64_t>(value);
			else if constexpr (std::is_signed_v<T> && std::is_integral_v<T>) v.IntValue = static_cast<int64_t>(value);
			else if constexpr (std::is_unsigned_v<T> && std::is_integral_v<T>) v.UIntValue = static_cast<uint64_t>(value);
			else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) v.FloatValue = static_cast<double>(value);
			else if constexpr (std::is_same_v<T, std::string>) v.StringValue = value;
			else if constexpr (std::is_same_v<T, glm::vec2>) { v.FloatVec[0] = value.x; v.FloatVec[1] = value.y; }
			else if constexpr (std::is_same_v<T, glm::vec3>) { v.FloatVec[0] = value.x; v.FloatVec[1] = value.y; v.FloatVec[2] = value.z; }
			else if constexpr (std::is_same_v<T, glm::vec4>) { v.FloatVec[0] = value.x; v.FloatVec[1] = value.y; v.FloatVec[2] = value.z; v.FloatVec[3] = value.w; }
			else if constexpr (std::is_same_v<T, glm::ivec2>) { v.IntVec[0] = value.x; v.IntVec[1] = value.y; }
			else if constexpr (std::is_same_v<T, glm::ivec3>) { v.IntVec[0] = value.x; v.IntVec[1] = value.y; v.IntVec[2] = value.z; }
			else if constexpr (std::is_same_v<T, glm::ivec4>) { v.IntVec[0] = value.x; v.IntVec[1] = value.y; v.IntVec[2] = value.z; v.IntVec[3] = value.w; }
			else if constexpr (std::is_same_v<T, Color>) { v.FloatVec[0] = value.r; v.FloatVec[1] = value.g; v.FloatVec[2] = value.b; v.FloatVec[3] = value.a; }
			return v;
		}

		template <typename T>
		void Unbox(const PropertyValue& v, T& out) {
			if constexpr (std::is_same_v<T, bool>) out = v.BoolValue;
			else if constexpr (std::is_enum_v<T>) out = static_cast<T>(v.IntValue);
			else if constexpr (std::is_signed_v<T> && std::is_integral_v<T>) out = static_cast<T>(v.IntValue);
			else if constexpr (std::is_unsigned_v<T> && std::is_integral_v<T>) out = static_cast<T>(v.UIntValue);
			else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) out = static_cast<T>(v.FloatValue);
			else if constexpr (std::is_same_v<T, std::string>) out = v.StringValue;
			else if constexpr (std::is_same_v<T, glm::vec2>) { out.x = v.FloatVec[0]; out.y = v.FloatVec[1]; }
			else if constexpr (std::is_same_v<T, glm::vec3>) { out.x = v.FloatVec[0]; out.y = v.FloatVec[1]; out.z = v.FloatVec[2]; }
			else if constexpr (std::is_same_v<T, glm::vec4>) { out.x = v.FloatVec[0]; out.y = v.FloatVec[1]; out.z = v.FloatVec[2]; out.w = v.FloatVec[3]; }
			else if constexpr (std::is_same_v<T, glm::ivec2>) { out.x = v.IntVec[0]; out.y = v.IntVec[1]; }
			else if constexpr (std::is_same_v<T, glm::ivec3>) { out.x = v.IntVec[0]; out.y = v.IntVec[1]; out.z = v.IntVec[2]; }
			else if constexpr (std::is_same_v<T, glm::ivec4>) { out.x = v.IntVec[0]; out.y = v.IntVec[1]; out.z = v.IntVec[2]; out.w = v.IntVec[3]; }
			else if constexpr (std::is_same_v<T, Color>) { out.r = v.FloatVec[0]; out.g = v.FloatVec[1]; out.b = v.FloatVec[2]; out.a = v.FloatVec[3]; }
		}

		template <typename TEnum>
		std::shared_ptr<EnumDescriptor> BuildEnumDescriptor(bool isFlags) {
			static_assert(std::is_enum_v<TEnum>, "BuildEnumDescriptor requires an enum type.");
			auto desc = std::make_shared<EnumDescriptor>();
			desc->IsFlags = isFlags;
			for (TEnum value : magic_enum::enum_values<TEnum>()) {
				auto name = magic_enum::enum_name(value);
				if (name.empty()) continue;
				EnumOption opt;
				opt.Name.assign(name.data(), name.size());
				opt.Value = static_cast<int64_t>(value);
				desc->Options.push_back(std::move(opt));
			}
			return desc;
		}

	} // namespace detail

	namespace Meta {

		inline PropertyMetadata Clamp(double min, double max) {
			return PropertyMetadata{}.WithClamp(min, max);
		}
		inline PropertyMetadata Clamp(double min, double max, float dragSpeed) {
			return PropertyMetadata{}.WithClamp(min, max).WithDragSpeed(dragSpeed);
		}

		inline PropertyMetadata DragSpeed(float speed) {
			return PropertyMetadata{}.WithDragSpeed(speed);
		}

		inline PropertyMetadata Tooltip(std::string text) {
			return PropertyMetadata{}.WithTooltip(std::move(text));
		}

		inline PropertyMetadata ReadOnly() {
			return PropertyMetadata{}.WithReadOnly(true);
		}

		inline PropertyMetadata NoSteppers() {
			return PropertyMetadata{}.WithHideStepperButtons(true);
		}

		inline PropertyMetadata Header(std::string content, int size = 5) {
			return PropertyMetadata{}.WithHeader(std::move(content), size);
		}

		inline PropertyMetadata Space(float h) {
			return PropertyMetadata{}.WithSpace(h);
		}

		inline PropertyMetadata MultiLine(int rows = 4) {
			return PropertyMetadata{}.WithMultiLine(rows);
		}

		template <typename TComponent>
		PropertyMetadata EnabledIf(std::function<bool(const TComponent&)> predicate) {
			std::function<bool(const Entity&)> wrapped =
				[predicate = std::move(predicate)](const Entity& e) -> bool {
					if (!e.HasComponent<TComponent>()) return false;
					return predicate(e.GetComponent<TComponent>());
				};
			return PropertyMetadata{}.WithEnabledIf(std::move(wrapped));
		}

		inline PropertyMetadata EnabledIf(std::function<bool(const Entity&)> predicate) {
			return PropertyMetadata{}.WithEnabledIf(std::move(predicate));
		}

	} // namespace Meta

	template <typename TComponent, typename TField>
	PropertyDescriptor Make(const std::string& name, const std::string& displayName,
		TField TComponent::* member, PropertyMetadata metadata = {})
	{
		// Fail loudly at compile time instead of silently registering a field that
		// deduces to PropertyType::None and then never renders in the inspector.
		static_assert(detail::DeducePropertyType<TField>() != PropertyType::None,
			"Properties::Make: unsupported field type (DeducePropertyType returned None). "
			"Add a branch to DeducePropertyType, or use a typed Make* helper.");
		PropertyDescriptor desc;
		desc.Name = name;
		desc.DisplayName = displayName.empty() ? name : displayName;
		desc.Type = detail::DeducePropertyType<TField>();
		desc.Metadata = std::move(metadata);
		if constexpr (std::is_enum_v<TField>) {
			desc.Metadata.Enum = detail::BuildEnumDescriptor<TField>(/*isFlags=*/false);
		}

		desc.Get = [member](const Entity& e) -> PropertyValue {
			const auto& component = e.GetComponent<TComponent>();
			return detail::Box<TField>(component.*member);
		};
		desc.Set = [member](Entity& e, const PropertyValue& v) {
			auto& component = e.GetComponent<TComponent>();
			detail::Unbox<TField>(v, component.*member);
		};
		return desc;
	}

	template <typename TComponent, typename TEnum>
	PropertyDescriptor MakeFlagEnum(const std::string& name, const std::string& displayName,
		TEnum TComponent::* member, PropertyMetadata metadata = {})
	{
		static_assert(std::is_enum_v<TEnum>, "MakeFlagEnum requires an enum type.");
		PropertyDescriptor desc;
		desc.Name = name;
		desc.DisplayName = displayName.empty() ? name : displayName;
		desc.Type = PropertyType::FlagEnum;
		desc.Metadata = std::move(metadata);
		desc.Metadata.Enum = detail::BuildEnumDescriptor<TEnum>(/*isFlags=*/true);

		desc.Get = [member](const Entity& e) -> PropertyValue {
			PropertyValue v;
			v.Type = PropertyType::FlagEnum;
			const auto& component = e.GetComponent<TComponent>();
			v.IntValue = static_cast<int64_t>(component.*member);
			return v;
		};
		desc.Set = [member](Entity& e, const PropertyValue& v) {
			auto& component = e.GetComponent<TComponent>();
			component.*member = static_cast<TEnum>(v.IntValue);
		};
		return desc;
	}

	// Getter/setter-driven field for when the setter has side effects (re-uploading to physics, GL state, etc.).
	template <typename TField, typename Getter, typename Setter>
	PropertyDescriptor MakeWith(const std::string& name, const std::string& displayName,
		Getter getter, Setter setter, PropertyMetadata metadata = {})
	{
		PropertyDescriptor desc;
		desc.Name = name;
		desc.DisplayName = displayName.empty() ? name : displayName;
		desc.Type = detail::DeducePropertyType<TField>();
		desc.Metadata = std::move(metadata);
		if constexpr (std::is_enum_v<TField>) {
			desc.Metadata.Enum = detail::BuildEnumDescriptor<TField>(/*isFlags=*/false);
		}
		desc.Get = [getter](const Entity& e) -> PropertyValue {
			return detail::Box<TField>(getter(e));
		};
		desc.Set = [setter](Entity& e, const PropertyValue& v) {
			TField field{};
			detail::Unbox<TField>(v, field);
			setter(e, field);
		};
		return desc;
	}

	// Texture reference field. Setter may be slice-aware (void(Entity&, uint64_t, const std::string&)) or plain (void(Entity&, uint64_t)); dispatch is if constexpr.
	template <typename Getter, typename Setter>
	PropertyDescriptor MakeTextureRef(const std::string& name, const std::string& displayName,
		Getter getter, Setter setter, PropertyMetadata metadata = {})
	{
		PropertyDescriptor desc;
		desc.Name = name;
		desc.DisplayName = displayName.empty() ? name : displayName;
		desc.Type = PropertyType::TextureRef;
		desc.Metadata = std::move(metadata);
		desc.Get = [getter](const Entity& e) -> PropertyValue {
			PropertyValue v;
			v.Type = PropertyType::TextureRef;
			v.UIntValue = static_cast<uint64_t>(getter(e));
			return v;
		};
		desc.Set = [setter](Entity& e, const PropertyValue& v) {
			if constexpr (std::is_invocable_v<Setter, Entity&, uint64_t, const std::string&>) {
				setter(e, v.UIntValue, v.StringValue);
			}
			else {
				setter(e, v.UIntValue);
			}
		};
		return desc;
	}

	template <typename Getter, typename Setter>
	PropertyDescriptor MakeAudioRef(const std::string& name, const std::string& displayName,
		Getter getter, Setter setter, PropertyMetadata metadata = {})
	{
		PropertyDescriptor desc;
		desc.Name = name;
		desc.DisplayName = displayName.empty() ? name : displayName;
		desc.Type = PropertyType::AudioRef;
		desc.Metadata = std::move(metadata);
		desc.Get = [getter](const Entity& e) -> PropertyValue {
			PropertyValue v;
			v.Type = PropertyType::AudioRef;
			v.UIntValue = static_cast<uint64_t>(getter(e));
			return v;
		};
		desc.Set = [setter](Entity& e, const PropertyValue& v) {
			setter(e, v.UIntValue);
		};
		return desc;
	}

	template <typename Getter, typename Setter>
	PropertyDescriptor MakeStringList(const std::string& name, const std::string& displayName,
		Getter getter, Setter setter, PropertyMetadata metadata = {})
	{
		PropertyDescriptor desc;
		desc.Name = name;
		desc.DisplayName = displayName.empty() ? name : displayName;
		desc.Type = PropertyType::StringList;
		desc.Metadata = std::move(metadata);
		desc.Get = [getter](const Entity& e) -> PropertyValue {
			PropertyValue v;
			v.Type = PropertyType::StringList;
			v.StringListValue = getter(e);
			return v;
		};
		desc.Set = [setter](Entity& e, const PropertyValue& v) {
			setter(e, v.StringListValue);
		};
		return desc;
	}

	template <typename TItem, typename Getter, typename Setter>
	PropertyDescriptor MakeListWith(const std::string& name, const std::string& displayName,
		Getter getter, Setter setter, PropertyMetadata metadata = {})
	{
		static_assert(detail::DeducePropertyType<TItem>() != PropertyType::None,
			"MakeListWith requires a primitive item type (bool/int/float/string/vec/color/enum).");
		PropertyDescriptor desc;
		desc.Name = name;
		desc.DisplayName = displayName.empty() ? name : displayName;
		desc.Type = PropertyType::List;
		desc.Metadata = std::move(metadata);
		desc.Metadata.ListItemType = detail::DeducePropertyType<TItem>();
		if constexpr (std::is_enum_v<TItem>) {
			desc.Metadata.Enum = detail::BuildEnumDescriptor<TItem>(/*isFlags=*/false);
		}
		desc.Get = [getter](const Entity& e) -> PropertyValue {
			PropertyValue v;
			v.Type = PropertyType::List;
			std::vector<TItem> items = getter(e);
			v.ListValue.reserve(items.size());
			for (const TItem& item : items) {
				v.ListValue.push_back(detail::Box<TItem>(item));
			}
			return v;
		};
		desc.Set = [setter](Entity& e, const PropertyValue& v) {
			std::vector<TItem> items;
			items.reserve(v.ListValue.size());
			for (const PropertyValue& boxed : v.ListValue) {
				TItem item{};
				detail::Unbox<TItem>(boxed, item);
				items.push_back(std::move(item));
			}
			setter(e, items);
		};
		return desc;
	}

	template <typename TComponent, typename TItem>
	PropertyDescriptor MakeList(const std::string& name, const std::string& displayName,
		std::vector<TItem> TComponent::* member, PropertyMetadata metadata = {})
	{
		return MakeListWith<TItem>(name, displayName,
			[member](const Entity& e) -> std::vector<TItem> {
				return e.GetComponent<TComponent>().*member;
			},
			[member](Entity& e, const std::vector<TItem>& items) {
				e.GetComponent<TComponent>().*member = items;
			},
			std::move(metadata));
	}

	template <typename Getter, typename Setter>
	PropertyDescriptor MakeEntityRef(const std::string& name, const std::string& displayName,
		Getter getter, Setter setter, PropertyMetadata metadata = {})
	{
		PropertyDescriptor desc;
		desc.Name = name;
		desc.DisplayName = displayName.empty() ? name : displayName;
		desc.Type = PropertyType::EntityRef;
		desc.Metadata = std::move(metadata);
		desc.Get = [getter](const Entity& e) -> PropertyValue {
			PropertyValue v;
			v.Type = PropertyType::EntityRef;
			v.UIntValue = static_cast<uint64_t>(getter(e));
			return v;
		};
		desc.Set = [setter](Entity& e, const PropertyValue& v) {
			setter(e, v.UIntValue);
		};
		return desc;
	}

	template <typename Getter, typename Setter>
	PropertyDescriptor MakeFontRef(const std::string& name, const std::string& displayName,
		Getter getter, Setter setter, PropertyMetadata metadata = {})
	{
		PropertyDescriptor desc;
		desc.Name = name;
		desc.DisplayName = displayName.empty() ? name : displayName;
		desc.Type = PropertyType::FontRef;
		desc.Metadata = std::move(metadata);
		desc.Get = [getter](const Entity& e) -> PropertyValue {
			PropertyValue v;
			v.Type = PropertyType::FontRef;
			v.UIntValue = static_cast<uint64_t>(getter(e));
			return v;
		};
		desc.Set = [setter](Entity& e, const PropertyValue& v) {
			setter(e, v.UIntValue);
		};
		return desc;
	}

	template <typename TTag>
	PropertyDescriptor::Branch Branch(TTag tagValue, std::vector<PropertyDescriptor> properties) {
		static_assert(std::is_enum_v<TTag>, "Branch tag must be an enum.");
		PropertyDescriptor::Branch branch;
		branch.TagValue = static_cast<int64_t>(tagValue);
		branch.Properties = std::move(properties);
		return branch;
	}

	template <typename TComponent, typename TTag>
	PropertyDescriptor MakeVariant(const std::string& name, const std::string& displayName,
		TTag TComponent::* tagMember, std::vector<PropertyDescriptor::Branch> branches,
		PropertyMetadata metadata = {})
	{
		static_assert(std::is_enum_v<TTag>, "MakeVariant tag must be an enum.");
		PropertyDescriptor desc = Make<TComponent, TTag>(name, displayName, tagMember, std::move(metadata));
		desc.VariantBranches = std::move(branches);
		return desc;
	}

	template <typename TTag, typename Getter, typename Setter>
	PropertyDescriptor MakeVariantWith(const std::string& name, const std::string& displayName,
		Getter getter, Setter setter, std::vector<PropertyDescriptor::Branch> branches,
		PropertyMetadata metadata = {})
	{
		static_assert(std::is_enum_v<TTag>, "MakeVariantWith tag must be an enum.");
		PropertyDescriptor desc = MakeWith<TTag>(name, displayName, getter, setter, std::move(metadata));
		desc.VariantBranches = std::move(branches);
		return desc;
	}

} // namespace Index::Properties
