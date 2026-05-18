#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

namespace Index {

	// Direction the scrollbar's value axis runs along. The handle slides
	// from the start of the chosen axis (Value=0) toward the end (Value=1).
	enum class ScrollbarDirection : int {
		LeftToRight = 0,
		RightToLeft = 1,
		BottomToTop = 2,
		TopToBottom = 3,
	};

	// Scrollbar widget — a draggable handle inside a track that produces a
	// normalised [0, 1] Value. Mirrors Unity's UI Scrollbar:
	//
	//   Value         — current position in [0, 1].
	//   Size          — handle's length as a fraction of the track in
	//                   [0, 1]. Drives the visual handle's anchor span on
	//                   the value axis.
	//   NumberOfSteps — when > 1, snaps Value to one of (NumberOfSteps - 1)
	//                   equal divisions across the track. 0 disables
	//                   snapping (smooth drag).
	//   Direction     — which way the value increases visually.
	//   HandleEntity  — child whose RectTransform2D the system rewrites
	//                   each frame to position + size the visual handle.
	//                   UIEventSystem auto-resolves a child named "Handle"
	//                   with an ImageComponent when this is unset.
	//
	// UIEventSystem owns the drag state machine. Pressing on the handle
	// snapshots the cursor + value, and dragging updates Value relative to
	// the press point so the handle follows the cursor smoothly. Pressing
	// on the empty track jumps the handle by one Size-worth of value (Unity
	// "page" click). Read-only scrollbars accept hover / press visuals but
	// never mutate Value from input.
	//
	// ScrollRectComponent uses a Scrollbar entity for its horizontal /
	// vertical scrollbars; mutations on either side feed each other so the
	// handle position reflects the content offset and dragging the
	// scrollbar scrolls the content.
	struct ScrollbarComponent {
		float Value = 0.0f;            // [0, 1]
		float Size  = 0.2f;            // handle fraction of track in [0, 1]
		int   NumberOfSteps = 0;       // 0 = smooth, >1 = snapped

		ScrollbarDirection Direction = ScrollbarDirection::LeftToRight;

		// Read-only scrollbars still update visual hover / press but their
		// Value is never changed by input. Programmatic writes still work.
		bool IsReadOnly = false;

		EntityHandle HandleEntity = kNullEntity;

		// Set by UIEventSystem on the frame Value changed (drag, page-click,
		// programmatic write). Cleared at the start of the next tick so
		// callers don't have to remember a "previous value" themselves.
		bool ValueChangedThisFrame = false;

		// Diff baseline for the change-detection above. Mirrors Slider's
		// LastObservedValue / ValueObserved pattern. Transient — not
		// serialized.
		float LastObservedValue = 0.0f;
		bool ValueObserved = false;

		// Per-state visuals applied to the handle's ImageComponent (see
		// ButtonComponent for the same TransitionMode + per-state sprite
		// model).
		UITransitionMode TransitionMode = UITransitionMode::ColorTint;

		Color NormalColor   { 1.00f, 1.00f, 1.00f, 1.0f };
		Color HoveredColor  { 0.85f, 0.85f, 0.85f, 1.0f };
		Color PressedColor  { 0.65f, 0.65f, 0.65f, 1.0f };
		Color DisabledColor { 0.50f, 0.50f, 0.50f, 0.5f };
		Color FocusedColor  { 0.00f, 0.00f, 0.00f, 0.0f };

		UUID NormalSprite  { 0 };
		UUID HoveredSprite { 0 };
		UUID PressedSprite { 0 };
		UUID DisabledSprite{ 0 };
		UUID FocusedSprite { 0 };

		// Transient drag state. PressMouseAxis is the cursor coordinate
		// (UI space) along the value axis when the handle was first
		// pressed; PressValue is Value at that moment. IsDragging stays
		// false until the cursor has moved a small threshold so a tap
		// doesn't snap the value to the cursor's exact position.
		float PressMouseAxis = 0.0f;
		float PressValue = 0.0f;
		bool IsDragging = false;
	};

}
