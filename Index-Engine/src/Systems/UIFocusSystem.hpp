#pragma once
#include "Scene/EntityHandle.hpp"
#include "Scene/ISystem.hpp"

#include <entt/entt.hpp>

namespace Index {

	// Optional keyboard / gamepad focus-navigation pass for the UI.
	// Runs BEFORE UIEventSystem each frame so its mouse-focus promotion
	// uses fresh resolved rects from UILayoutSystem and so its Activate
	// flag (InteractableComponent::ActivatedThisFrame) is in place when
	// UIEventSystem's hit-test loop runs and synthesises a click.
	//
	// Opt-in semantics — only entities whose InteractableComponent has
	// Focusable = true participate in navigation. The system writes
	// IsFocused on every InteractableComponent it sees (true on the one
	// focused entity, false on all others), so widgets that opt out
	// (the default) are guaranteed to read IsFocused == false and behave
	// exactly like the mouse-only path.
	//
	// Bindings (hardcoded for now — exposed as a future input-action
	// rebinding layer):
	//   Tab / D-pad-Down / left-stick-down / down-arrow → next
	//   Shift+Tab / D-pad-Up / left-stick-up / up-arrow → previous
	//   D-pad-Left / left-stick-left / left-arrow       → previous
	//   D-pad-Right / left-stick-right / right-arrow    → next
	//   Enter / Space / Gamepad-A                       → activate
	//   Esc / Gamepad-B                                 → cancel (clear focus)
	//
	// When the focused widget is an InputField the system surrenders
	// arrow keys to UIEventSystem's caret handler — Tab and gamepad
	// still navigate, so the user can always escape the field with
	// the keyboard. Clicking with the mouse on a focusable widget
	// also moves focus there (so mouse + keyboard mix naturally).
	class UIFocusSystem : public ISystem {
	public:
		void Update(Scene& scene) override;

	private:
		// Persistent across frames so Tab navigation is sticky between
		// updates. Cleared on Cancel and when the entity is destroyed,
		// disabled, or has its Focusable flag turned off at runtime.
		EntityHandle m_FocusedEntity = entt::null;

		// Used by left-stick / D-pad bindings to fire one nav step per
		// "deflection" (rising edge) instead of streaming a step every
		// frame the stick is pushed.
		bool m_PrevAxisLeftPushed  = false;
		bool m_PrevAxisRightPushed = false;
		bool m_PrevAxisUpPushed    = false;
		bool m_PrevAxisDownPushed  = false;
	};

}
