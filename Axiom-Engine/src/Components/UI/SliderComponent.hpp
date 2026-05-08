#pragma once

#include "Collections/Color.hpp"
#include "Scene/EntityHandle.hpp"

#include <entt/entt.hpp>

namespace Axiom {

	// Horizontal slider widget state. UIEventSystem owns the dragging
	// state machine: while the user holds the mouse down inside the
	// track's resolved rect, Value is updated every frame from the
	// cursor's X position relative to the track. Game code reads
	// Value (and the convenience flag ValueChangedThisFrame) instead of
	// re-implementing the drag math.
	//
	// HandleEntity is the optional child entity used as the visual thumb;
	// the slider system repositions it along the track every frame by
	// updating its RectTransform2D::AnchoredPosition.x. Leave it as
	// entt::null to draw the slider track-only (useful when the thumb
	// is implemented some other way, e.g. a fill bar).
	//
	// FillEntity is an optional child Image that grows/shrinks along the
	// X axis to visually represent the current value, like Unity's
	// "Fill Area / Fill" pair. The system rewrites its RectTransform2D
	// stretch each frame to span [0, t] of the track, where t is the
	// normalized [MinValue, MaxValue] position.
	struct SliderComponent {
		float Value = 0.5f;       // current value in [MinValue, MaxValue]
		float MinValue = 0.0f;
		float MaxValue = 1.0f;
		bool WholeNumbers = false;

		EntityHandle HandleEntity = entt::null;
		EntityHandle FillEntity = entt::null;

		// Set to true by UIEventSystem on the frame Value moved. Cleared
		// at the start of the next event-system tick so callers don't
		// have to remember a "previous value" themselves.
		bool ValueChangedThisFrame = false;
	};

}
