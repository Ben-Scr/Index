#pragma once

#include "Inspector/PropertyDescriptor.hpp"
#include "Scene/Entity.hpp"

#include <span>
#include <string>

namespace Index::PropertyDrawer {

	// `fieldKey` disambiguates reference-picker callbacks across frames; use "<componentDisplayName>.<fieldName>" for native components.
	// Returns true if the user changed the value this frame.
	bool Draw(std::span<const Entity> entities, const PropertyDescriptor& descriptor,
		const std::string& fieldKey);

	// Convenience overload: builds a key from a caller-supplied prefix +
	// descriptor.Name.
	bool DrawWithPrefix(std::span<const Entity> entities,
		const PropertyDescriptor& descriptor, const std::string& fieldKeyPrefix);

	void DrawAll(std::span<const Entity> entities,
		std::span<const PropertyDescriptor> descriptors,
		const std::string& fieldKeyPrefix);

} // namespace Index::PropertyDrawer
