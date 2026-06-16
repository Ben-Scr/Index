#pragma once

#include "Inspector/PropertyType.hpp"
#include "Assets/AssetKind.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Index {

	class Entity;

	struct PropertyMetadata {
		// Hover tooltip text (empty = no tooltip).
		std::string Tooltip;

		// Section header drawn ABOVE the property row. Renders as a separator
		// with a bigger font when HeaderSize > 0.
		std::string HeaderContent;
		int HeaderSize = 0;

		// Vertical spacer above the property row, in pixels.
		float SpaceHeight = 0.0f;
		bool HasSpace = false;

		// Numeric clamp / drag-speed.
		bool HasClamp = false;
		double ClampMin = 0.0;
		double ClampMax = 0.0;
		float DragSpeed = 0.1f;

		// Soft clamp for the DRAG-field path: keeps a normal DragFloat (no slider) but
		// clamps the dragged/typed value to [DragClampMin, DragClampMax]. Unlike HasClamp
		// (which switches the widget to a slider), this only bounds the value.
		bool HasDragClamp = false;
		double DragClampMin = 0.0;
		double DragClampMax = 0.0;

		// Whether the field is read-only in the inspector.
		bool ReadOnly = false;

		bool HideStepperButtons = false;

		// Vec*/IntVec* only: prefix each channel with an X/Y/Z/W axis label.
		bool ShowAxisLabels = false;

		bool MultiLine = false;
		int MultiLineRows = 4;

		// For PropertyType::Enum / FlagEnum.
		std::shared_ptr<EnumDescriptor> Enum;

		// For PropertyType::ComponentRef — the displayName of the required
		// ComponentInfo (matches the engine's ComponentInfo::displayName).
		std::string ComponentTypeName;

		// For PropertyType::AssetRef — restricts the picker to one AssetKind
		// (e.g. DataAsset). Unknown = any asset.
		AssetKind AssetKindFilter = AssetKind::Unknown;

		// TextureRef / List<TextureRef> only: hide sprite-sheet slice sub-entries
		// from the picker. Slices can't round-trip through a value that only
		// carries a UUID (script fields), so offering them would silently collapse
		// the pick to the full texture.
		bool SuppressTextureSlices = false;

		// For PropertyType::List — the PropertyType of each list item. The
		// drawer dispatches per-row through the matching primitive widget.
		PropertyType ListItemType = PropertyType::None;

		// For PropertyType::List with INLINE OBJECT items: the schema of each
		// element's leaf sub-fields, in encode/decode order. Non-empty means the
		// list holds inline objects (each element edited field-by-field) rather
		// than primitives/references; ListItemType is then unused.
		struct ListItemField {
			std::string Name;
			std::string DisplayName;
			PropertyType Type = PropertyType::None;
			std::shared_ptr<EnumDescriptor> Enum;
			std::string ComponentTypeName;
			AssetKind AssetKindFilter = AssetKind::Unknown;
		};
		std::vector<ListItemField> ListItemFields;

		std::function<bool(const Entity&)> EnabledIfFn;

		std::function<void(Entity&)> ResetSectionFn;

		// === Builder methods ===
		// Each returns *this so authors can chain.

		PropertyMetadata& WithClamp(double min, double max) {
			HasClamp = true;
			ClampMin = min;
			ClampMax = max;
			return *this;
		}
		PropertyMetadata& WithDragSpeed(float speed) {
			DragSpeed = speed;
			return *this;
		}
		PropertyMetadata& WithDragClamp(double min, double max) {
			HasDragClamp = true;
			DragClampMin = min;
			DragClampMax = max;
			return *this;
		}
		// Min-only drag clamp (no slider); upper bound effectively unbounded.
		PropertyMetadata& WithDragMin(double min) {
			return WithDragClamp(min, 1.0e30);
		}
		PropertyMetadata& WithTooltip(std::string text) {
			Tooltip = std::move(text);
			return *this;
		}
		PropertyMetadata& WithReadOnly(bool ro = true) {
			ReadOnly = ro;
			return *this;
		}
		PropertyMetadata& WithHideStepperButtons(bool hide = true) {
			HideStepperButtons = hide;
			return *this;
		}
		PropertyMetadata& WithAxisLabels(bool show = true) {
			ShowAxisLabels = show;
			return *this;
		}
		PropertyMetadata& WithHeader(std::string content, int size = 5) {
			HeaderContent = std::move(content);
			HeaderSize = size;
			return *this;
		}
		PropertyMetadata& WithSpace(float h) {
			HasSpace = true;
			SpaceHeight = h;
			return *this;
		}
		PropertyMetadata& WithMultiLine(int rows = 4) {
			MultiLine = true;
			MultiLineRows = rows;
			return *this;
		}
		PropertyMetadata& WithEnabledIf(std::function<bool(const Entity&)> pred) {
			EnabledIfFn = std::move(pred);
			return *this;
		}
		PropertyMetadata& WithResetSection(std::function<void(Entity&)> reset) {
			ResetSectionFn = std::move(reset);
			return *this;
		}
		PropertyMetadata& WithSuppressTextureSlices(bool suppress = true) {
			SuppressTextureSlices = suppress;
			return *this;
		}
	};

} // namespace Index
