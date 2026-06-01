#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

namespace Index {

	enum class SliderDirection : int {
		LeftToRight = 0,
		RightToLeft = 1,
		BottomToTop = 2,
		TopToBottom = 3,
	};

	// IMPORTANT: UIEventSystem OWNS FillEntity's RectTransform2D fields (AnchorMin/Max, Pivot, AnchoredPosition, SizeDelta) — they reset every tick. Style via ImageComponent instead. HandleEntity's AnchoredPosition is likewise owned; SizeDelta is not.
	struct SliderComponent {
		float Value = 0.5f;       // current value in [MinValue, MaxValue]
		float MinValue = 0.0f;
		float MaxValue = 1.0f;
		bool WholeNumbers = false;

		SliderDirection Direction = SliderDirection::LeftToRight;

		bool IsReadOnly = false;

		EntityHandle HandleEntity = kNullEntity;
		EntityHandle FillEntity = kNullEntity;
		EntityHandle BackgroundEntity = kNullEntity; // auto-resolved from child named "Background" when unset

		EntityHandle LabelEntity = kNullEntity; // when set, text is rewritten to the normalised percent each frame

		// Cleared at start of the next tick; diffs against LastObservedValue so any source (drag/inspector/script) fires OnValueChanged.
		bool ValueChangedThisFrame = false;

		float LastObservedValue = 0.0f; // baseline for next-tick diff; not serialized
		bool ValueObserved = false;     // false until first tick so deserialize doesn't fire a spurious event

		// Tinting/sprite swap applied to HandleEntity's ImageComponent (or slider parent's when no handle). Same TransitionMode model as ButtonComponent.
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

		// IsDragging is false until cursor moves past threshold (tap doesn't snap); Value tracks delta from PressMouseAxis, not absolute cursor.
		float PressMouseAxis = 0.0f;
		float PressValue = 0.0f;
		bool IsDragging = false;

		InspectorEventList OnValueChanged;
	};

}
