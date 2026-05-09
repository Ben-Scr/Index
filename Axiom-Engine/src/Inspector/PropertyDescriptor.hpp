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

namespace Axiom {

	// Describes ONE inspectable field on a component (or script).
	//
	// `Get(entity)` reads the field's current value into a PropertyValue.
	// `Set(entity, value)` writes a new value back. Both run per entity in a
	// multi-selection. Component-aware code lives entirely inside the
	// captured lambdas, so the drawer never has to know which native type
	// owns the field.
	//
	// Variant support: when `VariantBranches` is non-empty, this descriptor
	// acts as a discriminator. After drawing the primary widget (typically
	// an Enum combo), the drawer iterates the branches; for whichever
	// branch's TagValue matches the current discriminator value, the
	// branch's nested descriptors are drawn indented under the row. Mixed
	// discriminators across the selection render a "mixed variant" hint
	// instead of any branch — there's no sane way to share branch fields
	// across different shapes.
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

} // namespace Axiom
