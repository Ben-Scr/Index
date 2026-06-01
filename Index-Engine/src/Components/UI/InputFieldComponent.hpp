#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

#include <cstdint>
#include <string>

namespace Index {

	// Filters typed input + paste only; programmatic Text writes bypass filtering.
	enum class InputContentType : std::uint8_t {
		Standard,        // accept everything printable
		AlphaNumeric,    // [A-Za-z0-9]
		Alpha,           // [A-Za-z]
		IntegerNumber,   // [0-9] plus a single leading '-'
		DecimalNumber,   // [0-9] plus single leading '-' and single '.'
	};

	struct InputFieldComponent {
		std::string Text;
		std::string PlaceholderText = "Enter text...";

		bool IsFocused = false;
		bool SubmittedThisFrame = false;     // Enter pressed while focused
		int CharacterLimit = 0;              // 0 = unlimited

		InputContentType ContentType = InputContentType::Standard;
		bool IsSecret = false;
		// Read-only still allows focus/scroll/Copy/SelectAll; rejects Backspace/Delete/type/Cut/Paste. Visual styling unchanged.
		bool IsReadOnly = false;
		float CaretBlinkRate = 1.0f;

		// Caret bar width in pixels. Range exposed in the inspector
		// clamps to [1, 5]; the renderer enforces a >= 1 floor too.
		float CaretWidth = 1.0f;

		// UTF-8 byte offsets; range=[min,max); Caret==Anchor = no selection (typing inserts rather than replaces).
		int CaretBytePos = 0;
		int SelectionAnchorBytePos = 0;

		// Transient (not serialized). Hold timers drive auto-repeat for Backspace/Delete/Left/Right; without arrow timers, holding only moved one codepoint.
		bool MouseSelecting = false;
		float BackspaceHoldTime = 0.0f;
		float BackspaceRepeatAccumulator = 0.0f;
		float DeleteHoldTime = 0.0f;
		float DeleteRepeatAccumulator = 0.0f;
		float LeftHoldTime = 0.0f;
		float LeftRepeatAccumulator = 0.0f;
		float RightHoldTime = 0.0f;
		float RightRepeatAccumulator = 0.0f;

		EntityHandle TextEntity = kNullEntity;

		// Tint applied to TextEntity when Text is non-empty or the field
		// is focused (so a caret-visible empty focused field also uses
		// TextColor).
		Color TextColor{ 0.10f, 0.10f, 0.10f, 1.0f };
		// Lighter tint applied when Text is empty AND not focused, so the
		// placeholder reads as "hint" rather than "real value".
		Color PlaceholderColor{ 0.55f, 0.55f, 0.55f, 1.0f };
		// Background tint for the selection-highlight quad. Alpha < 1 so
		// the underlying text stays readable through the highlight.
		Color SelectionColor{ 0.30f, 0.55f, 0.95f, 0.45f };
		// Color of the blinking caret bar.
		Color CaretColor{ 0.10f, 0.10f, 0.10f, 1.0f };

		// Per-state background tints; text/placeholder/selection/caret colors are independent. Same TransitionMode+sprite model as ButtonComponent.
		UITransitionMode TransitionMode = UITransitionMode::ColorSwap;

		Color NormalColor   { 1.00f, 1.00f, 1.00f, 1.0f };
		Color HoveredColor  { 0.95f, 0.95f, 0.95f, 1.0f };
		Color PressedColor  { 0.90f, 0.90f, 0.90f, 1.0f };
		Color DisabledColor { 0.60f, 0.60f, 0.60f, 0.5f };
		// Alpha == 0 = "no focus tint" sentinel. See ButtonComponent.
		Color FocusedColor  { 0.00f, 0.00f, 0.00f, 0.0f };

		UUID NormalSprite  { 0 };
		UUID HoveredSprite { 0 };
		UUID PressedSprite { 0 };
		UUID DisabledSprite{ 0 };
		UUID FocusedSprite { 0 };

		InspectorEventList OnValueChanged;
		InspectorEventList OnSubmitted;

		// Initialized on first observation to suppress spurious OnValueChanged after deserialization. Transient — not serialized.
		std::string LastObservedText;
		bool ValueObserved = false;
	};

}
