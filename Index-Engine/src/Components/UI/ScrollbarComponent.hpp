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
		UITransitionMode TransitionMode = UITransitionMode::ColorSwap;

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

		float PressMouseAxis = 0.0f;
		float PressValue = 0.0f;
		bool IsDragging = false;
	};

}
