#pragma once

namespace Index {

	// Drives one frame of UI input state for an entity that wants to react
	// to mouse events. UIEventSystem updates these flags every frame from
	// the global Input state plus a hit-test against the entity's
	// RectTransform2D bounds. Game code (scripts, native systems) reads them
	// for the same kind of "if hovered, show tooltip" / "if clicked, do
	// thing" logic that's idiomatic in any UI toolkit.
	//
	// State semantics — exactly mirrors Unity's IPointerXxxHandler shape so
	// the mental model carries over:
	//
	//   IsHovered      — cursor is currently over this rect (this frame).
	//   IsMouseDown    — left button went DOWN this frame while hovering.
	//                    One-frame edge event; consumed before next frame.
	//   IsClicked      — left button went UP this frame while hovering AND
	//                    the original press was on this same entity. This
	//                    is the "real" click event — same semantics as
	//                    button widgets in every UI lib.
	//   IsMouseUp      — left button went UP this frame on the entity that
	//                    received the matching IsMouseDown, regardless of
	//                    where the cursor is now. Pairs 1:1 with IsMouseDown
	//                    so drag-style widgets (joysticks, scrubbers)
	//                    reliably see "let go" even if the cursor left the
	//                    rect. For the hover-gated completion event (press
	//                    AND release while still over the widget), use
	//                    IsClicked instead.
	//   IsPressed      — sticky: true between mouse-down and mouse-up if
	//                    the press started on this entity. Lets sliders /
	//                    drags track without re-checking every frame.
	//
	// Set Interactable = false to opt out of input entirely (greyed-out
	// disabled state, modal blocking, etc.) without removing the component.
	//
	// ── Optional focus / selection navigation ───────────────────────
	// Focusable defaults to false, so existing scenes get NO change in
	// behaviour — the widget stays mouse-only. Set Focusable = true to
	// opt this widget into UIFocusSystem's keyboard / controller
	// navigation:
	//   - Tab / Shift+Tab cycles forward / backward through focusables.
	//   - Arrow keys, D-pad and left-stick (any connected gamepad) do
	//     the same. Left/Right are reinterpreted as value adjustment
	//     when the focused widget is a Slider, and the input field
	//     reserves Left/Right/Up/Down for caret movement when one is
	//     focused.
	//   - Enter / Space / Gamepad-A "activates" — UIFocusSystem stamps
	//     IsClicked + IsMouseDown on the focused entity for one frame
	//     so every existing widget reaction (Button click, Toggle flip,
	//     Dropdown open, InputField submit) just works.
	//   - Esc / Gamepad-B clears focus.
	// IsFocused is the runtime flag the system writes each frame; it
	// pairs with FocusedColor on each widget preset (see those structs)
	// for the visual tint, which is itself opt-in via alpha > 0.
	struct InteractableComponent {
		bool Interactable = true;

		bool IsHovered = false;
		bool IsMouseDown = false;
		bool IsClicked = false;
		bool IsMouseUp = false;
		bool IsPressed = false;

		// Opt-in: when true, UIFocusSystem includes this widget in
		// keyboard / controller navigation. Default false preserves
		// mouse-only behaviour for scenes that don't want navigation.
		bool Focusable = false;

		// Runtime flag set by UIFocusSystem when this entity is the
		// currently focused widget. Read by widget tinting (gated on
		// FocusedColor.a > 0) and by GuiRenderer for any focus-ring
		// pass scripts choose to add. Writes to IsFocused from script
		// or inspector are honoured for one frame, then UIFocusSystem
		// reconciles it next tick.
		bool IsFocused = false;

		// Set by UIFocusSystem when an Activate input action (Enter /
		// Space / Gamepad-A) lands on this focused widget. UIEventSystem
		// runs immediately afterwards and synthesises the same flags
		// it would have written for a real mouse click — IsClicked,
		// IsMouseDown, IsPressed — so every existing widget reaction
		// (Button click, Toggle flip, Dropdown open, InputField submit)
		// works without each of them needing to learn about focus.
		// Cleared by UIEventSystem after consumption so it's strictly
		// one-frame-lived, just like the mouse edge flags above.
		bool ActivatedThisFrame = false;
	};

}
