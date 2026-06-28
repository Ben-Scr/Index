#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Components/UI/ScrollRectComponent.hpp"
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
		// 0 = unlimited. Max number of lines a Multiline field accepts (Enter / paste);
		// ignored when Multiline is false.
		int LineLimit = 0;

		InputContentType ContentType = InputContentType::Standard;
		bool IsSecret = false;
		// Read-only still allows focus/scroll/Copy/SelectAll; rejects Backspace/Delete/type/Cut/Paste. Visual styling unchanged.
		bool IsReadOnly = false;
		// Enter inserts a newline instead of submitting, paste keeps newlines, and Up/Down navigate visual lines.
		bool Multiline = false;
		float CaretBlinkRate = 1.0f;

		// Caret bar width in pixels. Range exposed in the inspector
		// clamps to [1, 5]; the renderer enforces a >= 1 floor too.
		float CaretWidth = 1.0f;

		// UTF-8 byte offsets; range=[min,max); Caret==Anchor = no selection (typing inserts rather than replaces).
		int CaretBytePos = 0;
		int SelectionAnchorBytePos = 0;

		// Transient (not serialized). Hold timers drive auto-repeat for Backspace/Delete/Left/Right; without arrow timers, holding only moved one codepoint.
		bool MouseSelecting = false;
		// Mouse-down position (UI px) for the drag-select threshold: extending the
		// selection only begins once the cursor moves past a few pixels, so a jittery
		// click doesn't leave a tiny unintended selection the next key would replace.
		float MouseDownX = 0.0f;
		float MouseDownY = 0.0f;
		float BackspaceHoldTime = 0.0f;
		float BackspaceRepeatAccumulator = 0.0f;
		float DeleteHoldTime = 0.0f;
		float DeleteRepeatAccumulator = 0.0f;
		float LeftHoldTime = 0.0f;
		float LeftRepeatAccumulator = 0.0f;
		float RightHoldTime = 0.0f;
		float RightRepeatAccumulator = 0.0f;
		float UpHoldTime = 0.0f;
		float UpRepeatAccumulator = 0.0f;
		float DownHoldTime = 0.0f;
		float DownRepeatAccumulator = 0.0f;

		// Goal column for vertical (Up/Down) navigation: the caret's within-line X in
		// UI pixels. -1 = recompute from the current caret. Reset to -1 on any horizontal
		// caret change so column tracking only persists across consecutive Up/Down presses.
		float CaretDesiredX = -1.0f;

		// Transient horizontal text scroll in UI pixels. The rendered text, caret and
		// selection shift left by this so the caret stays inside a left-aligned field
		// whose text overflows. Recomputed each frame from the caret; not serialized.
		float ScrollOffsetX = 0.0f;

		// Transient vertical text scroll in UI pixels (Multiline + VerticalScrollbar
		// only). The text block and caret/selection shift UP by this so later lines
		// become visible; >= 0, clamped to the overflow each frame. Driven by the
		// caret (auto-follow), the mouse wheel, and the scrollbar thumb. Not serialized.
		float ScrollOffsetY = 0.0f;
		// Caret byte the vertical scroll last auto-followed. Following only re-centres on
		// the caret when it actually moves (typing / navigation), so the mouse wheel can
		// scroll freely without being yanked back to the caret. -1 = not yet observed.
		int   ScrollFollowCaretByte = -1;

		EntityHandle TextEntity = kNullEntity;

		// Multiline only: assign a UI Scrollbar entity (one with a ScrollbarComponent,
		// typically TopToBottom) to scroll the text vertically. When set, the text
		// top-anchors and clips to the field, scrolls on wheel / caret-follow / handle
		// drag, and this field drives the scrollbar's Value + handle Size. kNullEntity =
		// no scrollbar (the field renders all lines as before).
		EntityHandle VerticalScrollbarEntity = kNullEntity;
		// Visibility rule for the assigned scrollbar (canonical ScrollbarVisibility):
		// Permanent always shows it; AutoHide / AutoHideAndExpandViewport hide it while
		// the text fits. The InputField doesn't inset its text for the bar, so the two
		// AutoHide variants behave the same here. No effect without a scrollbar assigned.
		ScrollbarVisibility VerticalScrollbarVisibility = ScrollbarVisibility::AutoHide;
		// Mouse-wheel scroll speed for a scrollable multiline field, in text lines per
		// wheel notch (0 = wheel disabled). Caret-follow and handle drag are unaffected.
		float ScrollSensitivity = 3.0f;

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
