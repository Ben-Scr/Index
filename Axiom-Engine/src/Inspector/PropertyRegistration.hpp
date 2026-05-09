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

// =============================================================================
// PropertyRegistration: deduce PropertyType from C++ types and build
// PropertyDescriptors for native components in one line.
//
// Usage (build a vector of PropertyDescriptors and pass to RegisterComponent):
//
//     RegisterComponent<NameComponent>(sceneManager, "Name", ..., {
//         Properties::Make("Name", "Name", &NameComponent::Name),
//     });
//
// The pointer-to-member is enough: the field type, getter, and setter are
// deduced. For enums, magic_enum builds the EnumDescriptor automatically.
// For getter/setter-driven fields use Properties::MakeWith<TField>(...).
// For flag enums, use Properties::MakeFlagEnum.
// For asset references, use the explicit helpers (Properties::MakeTextureRef
// / Properties::MakeAudioRef) so the drawer knows which kind to filter for.
//
// Metadata is built declaratively — each `Properties::Meta::*` helper
// returns a fresh `PropertyMetadata` configured with one common shape, and
// the `PropertyMetadata::With*` chain methods compose more on top:
//
//     Properties::Make("Volume", "Volume", &AudioSourceComponent::Volume,
//         Properties::Meta::Clamp(0.0, 1.0).WithDragSpeed(0.01f));
//
//     Properties::Make("Gravity", "Gravity Vector", &Settings::Gravity,
//         Properties::Meta::EnabledIf<Settings>([](const Settings& s) {
//             return s.UseGravity;
//         }));
//
// Variant fields (one descriptor whose nested branches are picked by an
// enum) are built via `Properties::MakeVariant` / `MakeVariantWith`. The
// drawer renders the discriminator combo, then the matching branch's
// descriptors directly under it.
//
// List fields (a `std::vector<TItem>` exposed as a row-per-item editor)
// are built via `Properties::MakeList` for direct member access; for
// custom ownership use `MakeListWith`.
// =============================================================================

namespace Axiom::Properties {

	namespace detail {

		// Map a concrete C++ field type to the canonical PropertyType. Most
		// fields are deducible at compile time. References (TextureHandle,
		// AudioHandle, etc.) live in their own helper functions because they
		// need additional metadata (the asset kind).
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

		// Build the EnumDescriptor for an enum type via magic_enum, copying
		// the names + integer values once at registration time.
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

	// =========================================================================
	// Meta — declarative metadata builders. Each helper returns a fresh
	// `PropertyMetadata` configured with one common shape, and the
	// `PropertyMetadata::With*` chain methods compose more on top.
	// =========================================================================
	namespace Meta {

		// Numeric clamp. Drag-speed defaults to PropertyMetadata's default
		// (0.1f). Pass a third argument when the field needs a coarser /
		// finer drag than the default.
		inline PropertyMetadata Clamp(double min, double max) {
			return PropertyMetadata{}.WithClamp(min, max);
		}
		inline PropertyMetadata Clamp(double min, double max, float dragSpeed) {
			return PropertyMetadata{}.WithClamp(min, max).WithDragSpeed(dragSpeed);
		}

		// Drag-speed alone (no clamp). Editing can go above/below the start
		// value freely.
		inline PropertyMetadata DragSpeed(float speed) {
			return PropertyMetadata{}.WithDragSpeed(speed);
		}

		// Hover tooltip.
		inline PropertyMetadata Tooltip(std::string text) {
			return PropertyMetadata{}.WithTooltip(std::move(text));
		}

		// Read-only field.
		inline PropertyMetadata ReadOnly() {
			return PropertyMetadata{}.WithReadOnly(true);
		}

		// Section header drawn above the row.
		inline PropertyMetadata Header(std::string content, int size = 5) {
			return PropertyMetadata{}.WithHeader(std::move(content), size);
		}

		// Vertical space above the row.
		inline PropertyMetadata Space(float h) {
			return PropertyMetadata{}.WithSpace(h);
		}

		// Multi-line string editor (PropertyType::String only). Defaults to
		// 4 visible rows.
		inline PropertyMetadata MultiLine(int rows = 4) {
			return PropertyMetadata{}.WithMultiLine(rows);
		}

		// EnabledIf gate. The predicate is evaluated against each entity in
		// the multi-selection; if any returns false, the row renders
		// disabled. The `TComponent` overload reads the named component
		// directly so the user can write the predicate in terms of the
		// component value rather than going through Entity::GetComponent.
		template <typename TComponent>
		PropertyMetadata EnabledIf(std::function<bool(const TComponent&)> predicate) {
			std::function<bool(const Entity&)> wrapped =
				[predicate = std::move(predicate)](const Entity& e) -> bool {
					if (!e.HasComponent<TComponent>()) return false;
					return predicate(e.GetComponent<TComponent>());
				};
			return PropertyMetadata{}.WithEnabledIf(std::move(wrapped));
		}

		// Raw entity-keyed predicate — for cross-component gating where the
		// predicate has to reach into more than one component.
		inline PropertyMetadata EnabledIf(std::function<bool(const Entity&)> predicate) {
			return PropertyMetadata{}.WithEnabledIf(std::move(predicate));
		}

	} // namespace Meta

	// =========================================================================
	// Field descriptors
	// =========================================================================

	// Generic primitive / vector / enum / string field. Type is deduced from
	// the field's declared type via detail::DeducePropertyType.
	template <typename TComponent, typename TField>
	PropertyDescriptor Make(const std::string& name, const std::string& displayName,
		TField TComponent::* member, PropertyMetadata metadata = {})
	{
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

	// Flag-enum field. The drawer renders this as a multi-checkbox combo so
	// individual bits can be toggled.
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

	// Get/Set-driven field. Used when the component exposes a value via
	// methods (e.g. Rigidbody2D::GetPosition / SetPosition) rather than a
	// direct field, OR when the setter has side effects (re-uploading to
	// the physics body, writing through to GL state, etc.). PropertyType is
	// deduced from TField; for enums the EnumDescriptor is auto-built so
	// magic_enum sees the values.
	//
	// `getter` and `setter` are stored by value; capture by `[component]`
	// or by `[]` — they must be copyable. The drawer calls them per-entity
	// inside its multi-select span.
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

	// Texture reference field. Stores the asset's UUID in PropertyValue's
	// UIntValue; drawer dispatches to ReferencePicker with kind=Texture and
	// the thumbnail-grid layout. `getter` returns the current asset UUID
	// (uint64); `setter` receives the new UUID and is responsible for
	// reloading the texture handle, marking dirty, etc.
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
			setter(e, v.UIntValue);
		};
		return desc;
	}

	// Audio reference field. Same shape as MakeTextureRef but routes through
	// AssetKind::Audio so the picker filters to .wav/.mp3/.ogg/.flac.
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

	// Ordered list-of-strings field. The drawer renders one editable
	// row per entry plus an "add row" button at the end. `getter`
	// returns the current list by value (drawer reads it once per
	// frame); `setter` receives the full new list (drawer writes it
	// after any edit, add, remove, or reorder). Use this when the
	// inspector needs to author small string arrays (Dropdown.Options
	// is the canonical case).
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

	// Generic list-of-T field. T must be a primitive type that
	// `detail::DeducePropertyType<T>()` returns a non-`None` value for
	// (bool, integer / float types, glm::vec*, glm::ivec*, Color, std::string,
	// or an enum). The drawer renders one row per element using the same
	// per-type widget the standalone field would use, plus an "Add" button
	// and per-row "Remove" buttons.
	//
	// `getter` returns the current vector by value (drawer reads it once
	// per frame); `setter` receives the full new vector (drawer writes it
	// after any edit, add, remove). The drawer never mutates ListValue's
	// items in-place; it always rebuilds the vector before calling setter
	// so user code doesn't have to think about mid-iteration churn.
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

	// Convenience: when the list lives directly on the component as a
	// `std::vector<TItem>` member, deduce everything from the pointer-to-
	// member. Equivalent to writing the trivial getter/setter by hand.
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

	// Entity reference field. Stores the referent's runtime entity id in
	// PropertyValue's UIntValue (0 == unset). The reference picker renders
	// a scene-tree selector when clicked. Same getter/setter shape as the
	// asset helpers — `getter` returns the current entity id (uint64),
	// `setter` receives the new id (and is responsible for clearing or
	// reattaching anything that depended on the old reference).
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

	// Font reference field. Same shape as MakeTextureRef / MakeAudioRef but
	// routes through AssetKind::Font so the picker filters to .ttf / .otf.
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

	// =========================================================================
	// Variant fields
	//
	// A variant descriptor renders a discriminator combo at the top, then —
	// when the discriminator is uniform across the selection — the matching
	// branch's nested descriptors directly under the row (indented). Mixed
	// discriminators render a "(mixed)" hint instead of any branch.
	//
	// Two flavours:
	//   * `MakeVariant`     — the discriminator is an enum field on the
	//                          component (`TTag TComponent::*`).
	//   * `MakeVariantWith` — the discriminator is computed from a
	//                          getter/setter pair (e.g. mapped from a
	//                          std::variant via std::visit + tag swap).
	//
	// Each branch is built by `Branch<TTag>(value, { descriptors })`, where
	// `value` is the enum option that triggers the branch.
	// =========================================================================

	// Helper: build a single VariantBranch entry from an enum value.
	template <typename TTag>
	PropertyDescriptor::Branch Branch(TTag tagValue, std::vector<PropertyDescriptor> properties) {
		static_assert(std::is_enum_v<TTag>, "Branch tag must be an enum.");
		PropertyDescriptor::Branch branch;
		branch.TagValue = static_cast<int64_t>(tagValue);
		branch.Properties = std::move(properties);
		return branch;
	}

	// Variant whose discriminator is a direct enum field on the component.
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

	// Variant whose discriminator is computed via getter/setter (e.g. to
	// map a std::variant<A, B> to a tag enum and swap the variant alternative
	// in the setter).
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

} // namespace Axiom::Properties
