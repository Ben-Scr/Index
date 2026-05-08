#pragma once

#include "Collections/Color.hpp"
#include "Scene/EntityHandle.hpp"

#include <entt/entt.hpp>
#include <string>
#include <vector>

namespace Axiom {

	// Dropdown / combo box widget state. The button portion is the
	// entity itself; the option list is rendered as a popup by
	// UIRenderer when IsOpen is true (drawn as floating rows directly
	// below the button rect, so no child entity is required for the
	// option list).
	//
	// SelectedIndex is the persisted choice. SelectionChangedThisFrame
	// is set by UIEventSystem on the frame the user picks a new option,
	// then cleared at the start of the next tick.
	//
	// LabelEntity is an optional child TextRenderer the system writes
	// the currently-selected option name into every frame, so the closed
	// dropdown's button shows the active selection (mirroring the
	// "header" cell of a Unity / TMP dropdown).
	struct DropdownComponent {
		std::vector<std::string> Options;
		int SelectedIndex = 0;

		bool IsOpen = false;
		bool SelectionChangedThisFrame = false;

		// Child entity whose TextRendererComponent shows the current
		// selection on the dropdown button. entt::null disables the
		// auto-write (game code can drive it manually then).
		EntityHandle LabelEntity = entt::null;

		// Visual tuning for the popup option list (drawn by UIRenderer).
		float OptionRowHeight = 28.0f;
		Color PopupBackgroundColor{ 0.95f, 0.95f, 0.95f, 1.0f };
		Color OptionTextColor{ 0.10f, 0.10f, 0.10f, 1.0f };
		Color OptionHoverColor{ 0.85f, 0.90f, 1.00f, 1.0f };
	};

}
