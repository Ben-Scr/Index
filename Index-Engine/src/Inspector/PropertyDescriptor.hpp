#pragma once

#include "Inspector/PropertyMetadata.hpp"
#include "Inspector/PropertyType.hpp"
#include "Inspector/PropertyValue.hpp"
#include "Scene/Entity.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Index {

	// One inspectable field; Get/Set are per-entity lambdas — drawer never sees the native type.
	// When VariantBranches is non-empty this acts as a discriminator; mixed-selection variants render a "mixed variant" hint instead of any branch.
	struct PropertyDescriptor {
		struct Branch {
			int64_t TagValue = 0;
			std::vector<PropertyDescriptor> Properties;
		};

		std::string Name;          // Stable serialised name (matches the C# field name).
		std::string DisplayName;   // Pretty label shown in the inspector. Defaults to Name.
		PropertyType Type = PropertyType::None;
		PropertyMetadata Metadata;

		std::function<PropertyValue(const Entity&)> Get;
		std::function<void(Entity&, const PropertyValue&)> Set;

		// Optional. When non-empty, this descriptor renders its own widget
		// AND the matching branch's nested descriptors below it.
		std::vector<Branch> VariantBranches;
	};

} // namespace Index
