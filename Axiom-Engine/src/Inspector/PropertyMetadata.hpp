#pragma once

#include "Inspector/PropertyType.hpp"

#include <functional>
#include <memory>
#include <string>

namespace Axiom {

	class Entity;

	// Per-descriptor metadata. Optional fields default to "no override". The
	// drawer reads these to pick clamp ranges, drag speeds, tooltip strings,
	// section headers, etc.
	//
	// All numeric metadata is stored as double so a single struct works for
	// integer and floating-point clamps without templating.
	//
	// Builder methods (`WithClamp`, `WithTooltip`, ...) return *this so authors
	// can chain configuration in one expression. Free helpers in
	// `Properties::Meta` (PropertyRegistration.hpp) start a fresh metadata so
	// the common shapes read like English: `Properties::Meta::Clamp(0, 1)`,
	// `Properties::Meta::Header("Audio").WithSpace(8.0f)`, etc.
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

		// PropertyType::String only: when true the drawer renders a
		// resizable multi-line text box instead of the default single
		// line. MultiLineRows controls the visible row count (defaults
		// to 4 lines when MultiLine is set, ignored otherwise).
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

		// Optional gate. If set, evaluated against every entity in the
		// selection before drawing the field. If any entity returns false
		// the row is rendered through ImGui::BeginDisabled (greyed-out
		// widget, edits ignored). Use this for "enable field B only when
		// field A is set" patterns — declarative, reusable, and the same
		// gate works for native components and C# script fields.
		std::function<bool(const Entity&)> EnabledIfFn;

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
	};

} // namespace Axiom
