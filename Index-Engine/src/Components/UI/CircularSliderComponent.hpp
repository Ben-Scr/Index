#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

namespace Index {

	// Hit-test is annulus-based (cursor must be between innerRadius and outerRadius); donut hole is not clickable.
	struct CircularSliderComponent {
		float Value = 0.5f;
		float MinValue = 0.0f;
		float MaxValue = 1.0f;
		bool WholeNumbers = false;

		// Read-only sliders still draw and accept hover, but Value never
		// changes from user drag. Programmatic writes still work.
		bool IsReadOnly = false;

		// Arc shape.
		float StartAngleDegrees = 90.0f;   // top (12 o'clock) by default
		float SweepDegrees      = 360.0f;  // full ring
		bool  Clockwise         = true;
		float RingThickness     = 20.0f;
		int   RingSegments      = 64;      // approximation density

		// Visual.
		Color BackgroundColor{ 0.18f, 0.20f, 0.24f, 1.0f };
		Color FillColor      { 0.30f, 0.55f, 0.95f, 1.0f };

		EntityHandle HandleEntity = kNullEntity;

		// Set by UIEventSystem on the frame Value changed (drag,
		// programmatic write, inspector edit). Cleared at the start of
		// the next event-system tick. Mirrors the linear Slider contract.
		bool ValueChangedThisFrame = false;
		float LastObservedValue = 0.0f;
		bool ValueObserved = false;

		// Drag tracks angular delta from PressMouseAngle/PressValue so the handle doesn't snap to cursor angle.
		float PressMouseAngle = 0.0f;
		float PressValue = 0.0f;
		bool IsDragging = false;

		UITransitionMode TransitionMode = UITransitionMode::ColorSwap;

		Color NormalColor   { 0.95f, 0.95f, 0.95f, 1.0f };
		Color HoveredColor  { 0.80f, 0.80f, 0.80f, 1.0f };
		Color PressedColor  { 0.60f, 0.60f, 0.60f, 1.0f };
		Color DisabledColor { 0.50f, 0.50f, 0.50f, 0.5f };
		// Alpha == 0 = "no focus tint" sentinel (see ButtonComponent).
		Color FocusedColor  { 0.00f, 0.00f, 0.00f, 0.0f };

		UUID NormalSprite  { 0 };
		UUID HoveredSprite { 0 };
		UUID PressedSprite { 0 };
		UUID DisabledSprite{ 0 };
		UUID FocusedSprite { 0 };
	};

}
