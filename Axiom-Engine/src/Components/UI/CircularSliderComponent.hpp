#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

#include <entt/entt.hpp>

namespace Axiom {

	// Circular slider — a ring-shaped value control. Same value semantics
	// as the linear SliderComponent (Value / Min / Max / WholeNumbers /
	// IsReadOnly + ValueChangedThisFrame for one-frame edge events) but
	// the drag math is polar instead of axis-aligned and rendering is a
	// pair of arcs (background + fill) rather than two rectangles.
	//
	// Geometry
	//   The ring is centred on the entity's RectTransform2D pivot. Its
	//   outer radius is half the smaller of the rect's width/height; the
	//   inner radius is outer minus RingThickness, so a square 200×200
	//   rect with RingThickness=20 paints a 200 px diameter ring with a
	//   160 px diameter hole.
	//
	//   StartAngleDegrees is the angle (in standard math convention —
	//   0° = +X right, 90° = +Y up, etc.) where Value=Min sits. SweepDegrees
	//   controls how much of the ring the slider covers; 360 paints a
	//   full ring, 270 leaves a quarter-arc gap (useful for "C" shapes).
	//   Clockwise reverses the direction Value increases visually.
	//
	// Rendering is procedural (no texture / image required): GuiRenderer
	// emits a short triangle-strip-ish series of small quads tangent to
	// the ring, one for the background sweep and one for the [Min, Value]
	// fill. RingSegments controls the smoothness — 64 is plenty for a
	// 200 px ring; bump it if you scale up.
	//
	// Hit-testing is an annulus check (cursor distance from centre must
	// fall inside [innerRadius, outerRadius]) so the donut hole isn't
	// clickable. UIEventSystem's standard rect-bound hit-test runs first
	// to pick the topmost candidate; the annulus check then demotes the
	// hover when the cursor sits in the hole or outside the ring.
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

		// Optional child whose RectTransform2D::AnchoredPosition gets
		// rewritten each frame to sit on the ring at the current Value
		// angle, like the linear Slider's HandleEntity. entt::null skips
		// the auto-positioning. The handle's hit-test is independent —
		// give it its own InteractableComponent if you want clicking the
		// thumb to start a drag, else the user drags by clicking the ring.
		EntityHandle HandleEntity = entt::null;

		// Set by UIEventSystem on the frame Value changed (drag,
		// programmatic write, inspector edit). Cleared at the start of
		// the next event-system tick. Mirrors the linear Slider contract.
		bool ValueChangedThisFrame = false;
		float LastObservedValue = 0.0f;
		bool ValueObserved = false;

		// Transient drag state owned by UIEventSystem. PressMouseAngle is
		// the cursor's angle (in radians, atan2 around centre) at press
		// time and PressValue is Value at that moment, so dragging follows
		// the angular delta rather than snapping the handle to the
		// cursor's exact angle. IsDragging stays false until the cursor
		// has moved past a small threshold so a tap doesn't shift Value.
		float PressMouseAngle = 0.0f;
		float PressValue = 0.0f;
		bool IsDragging = false;

		// Handle visual feedback. Mirrors the linear SliderComponent's
		// per-state palette + transition mode so a circular slider's
		// thumb gets the same hover/press tint or sprite swap as every
		// other widget. UIEventSystem applies the resolved state to the
		// HandleEntity's ImageComponent each frame; in edit mode the
		// system pre-applies NormalColor so authoring the palette gives
		// immediate feedback. Defaults to ColorTint, matching Button etc.
		UITransitionMode TransitionMode = UITransitionMode::ColorTint;

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
