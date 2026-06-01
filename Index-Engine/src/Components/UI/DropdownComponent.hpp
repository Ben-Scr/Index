#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

#include <string>
#include <vector>

namespace Index {

	struct DropdownComponent {
		std::vector<std::string> Options;
		int SelectedIndex = 0;

		bool IsOpen = false;
		bool SelectionChangedThisFrame = false;

		// Transient; not serialized. Gates the first-tick diff so scene load doesn't fire spurious OnSelectedIndexChange.
		int LastObservedSelectedIndex = 0;
		bool SelectionObserved = false;

		// Blocks user input; visual hover/press still tracks; programmatic writes to IsOpen/SelectedIndex still work.
		bool IsReadOnly = false;

		// Child entity whose TextRendererComponent shows the current
		// selection on the dropdown button. kNullEntity disables the
		// auto-write (game code can drive it manually then).
		EntityHandle LabelEntity = kNullEntity;

		// Visual tuning for the popup option list (drawn by UIRenderer).
		float OptionRowHeight = 28.0f;
		Color PopupBackgroundColor{ 0.95f, 0.95f, 0.95f, 1.0f };
		Color OptionTextColor{ 0.10f, 0.10f, 0.10f, 1.0f };

		// Alpha == 0 = 'no override'; falls through to next-lower precedence. OptionHoverColor is the legacy name kept for scene compatibility.
		Color OptionNormalColor{ 0.00f, 0.00f, 0.00f, 0.0f };
		Color OptionHoverColor{ 0.85f, 0.90f, 1.00f, 1.0f };
		Color OptionPressedColor{ 0.00f, 0.00f, 0.00f, 0.0f };
		Color OptionSelectedColor{ 0.00f, 0.00f, 0.00f, 0.0f };

		UITransitionMode TransitionMode = UITransitionMode::ColorSwap;

		Color NormalColor   { 1.00f, 1.00f, 1.00f, 1.0f };
		Color HoveredColor  { 0.92f, 0.94f, 0.98f, 1.0f };
		Color PressedColor  { 0.80f, 0.85f, 0.95f, 1.0f };
		Color DisabledColor { 0.60f, 0.60f, 0.60f, 0.5f };
		// Alpha == 0 = "no focus tint" sentinel. See ButtonComponent.
		Color FocusedColor  { 0.00f, 0.00f, 0.00f, 0.0f };

		UUID NormalSprite  { 0 };
		UUID HoveredSprite { 0 };
		UUID PressedSprite { 0 };
		UUID DisabledSprite{ 0 };
		UUID FocusedSprite { 0 };

		InspectorEventList OnValueChanged;
	};

}
