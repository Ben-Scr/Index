#pragma once

#include "Inspector/PropertyType.hpp"

#include <functional>
#include <memory>
#include <string>

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

		// For PropertyType::List — the PropertyType of each list item. The
		// drawer dispatches per-row through the matching primitive widget.
		PropertyType ListItemType = PropertyType::None;

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
	};

} // namespace Index
