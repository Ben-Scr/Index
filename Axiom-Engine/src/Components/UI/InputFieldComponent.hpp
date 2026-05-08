#pragma once

#include "Collections/Color.hpp"
#include "Scene/EntityHandle.hpp"

#include <entt/entt.hpp>
#include <string>

namespace Axiom {

	// Text input widget state. Pairs with a TextRenderer child entity
	// (TextEntity) that displays Text (or PlaceholderText when Text is
	// empty and the field isn't focused). UIEventSystem owns the focus
	// state machine:
	//
	//   - Click inside the field's RectTransform2D → IsFocused = true
	//   - Click anywhere else → IsFocused = false
	//   - While focused, the system appends typed characters (from the
	//     OS char callback) to Text and treats Enter as a "submit"
	//     (sets SubmittedThisFrame).
	//   - Backspace removes the last character.
	//
	// The system writes the appropriate text into TextEntity's
	// TextRendererComponent every frame and tints it with TextColor or
	// PlaceholderColor depending on whether Text is empty + unfocused.
	struct InputFieldComponent {
		std::string Text;
		std::string PlaceholderText = "Enter text...";

		bool IsFocused = false;
		bool SubmittedThisFrame = false;     // Enter pressed while focused
		int CharacterLimit = 0;              // 0 = unlimited

		// Child entity carrying the TextRendererComponent that renders
		// either Text or PlaceholderText each frame. entt::null disables
		// the child-text path (the field still tracks Text in case the
		// game reads it directly).
		EntityHandle TextEntity = entt::null;

		// Tint applied to TextEntity when Text is non-empty or the field
		// is focused (so a caret-visible empty focused field also uses
		// TextColor).
		Color TextColor{ 0.10f, 0.10f, 0.10f, 1.0f };
		// Lighter tint applied when Text is empty AND not focused, so the
		// placeholder reads as "hint" rather than "real value".
		Color PlaceholderColor{ 0.55f, 0.55f, 0.55f, 1.0f };
	};

}
