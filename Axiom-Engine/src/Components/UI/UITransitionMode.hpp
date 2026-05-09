#pragma once

#include <cstdint>

namespace Axiom {

	// How a widget visually transitions between Normal / Hovered /
	// Pressed / Focused / Disabled states. Set per-widget via the
	// TransitionMode field on each widget preset (ButtonComponent,
	// ToggleComponent, etc.). Defaults to ColorTint, which preserves
	// the original mouse-only colour-swap behaviour for every existing
	// scene.
	//
	//   ColorTint  — write the per-state Color into ImageComponent.Color.
	//                Original behaviour, no texture changes.
	//   SpriteSwap — write the per-state UUID into ImageComponent's
	//                TextureAssetId / TextureHandle, leaving the Color
	//                untouched. State slots that are unset (UUID{0})
	//                fall back to NormalSprite, so a typical setup is
	//                "set NormalSprite as the authoritative default,
	//                then any subset of Hovered / Pressed / Focused /
	//                Disabled to override per-state". When NormalSprite
	//                itself is unset the system performs no swap, so
	//                the widget keeps whatever texture was authored on
	//                its ImageComponent.
	//   None       — no automatic transition, the widget only reacts to
	//                whatever script code does. Useful when game code
	//                drives the visual change from outside.
	enum class UITransitionMode : std::uint8_t {
		ColorTint  = 0,
		SpriteSwap = 1,
		None       = 2,
	};

}
