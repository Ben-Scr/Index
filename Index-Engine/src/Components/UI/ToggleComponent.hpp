#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

namespace Index {

	struct ToggleComponent {
		bool IsOn = false;

		bool IsReadOnly = false;

		EntityHandle CheckmarkEntity = kNullEntity;

		bool ValueChangedThisFrame = false;

		// ValueObserved gates the first tick so scene load doesn't fire a spurious event. Transient.
		bool LastObservedIsOn = false;
		bool ValueObserved = false;

		// See ButtonComponent — same TransitionMode + per-state sprite
		// model.
		UITransitionMode TransitionMode = UITransitionMode::ColorSwap;

		Color NormalColor   { 1.00f, 1.00f, 1.00f, 1.0f };
		Color HoveredColor  { 0.85f, 0.85f, 0.85f, 1.0f };
		Color PressedColor  { 0.65f, 0.65f, 0.65f, 1.0f };
		Color DisabledColor { 0.50f, 0.50f, 0.50f, 0.5f };
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
