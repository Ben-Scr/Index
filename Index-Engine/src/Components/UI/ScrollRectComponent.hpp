#pragma once

#include "Collections/Vec2.hpp"
#include "Scene/EntityHandle.hpp"

#include <entt/entt.hpp>

namespace Index {

	// How the scroll content reacts when dragged past the content bounds.
	enum class ScrollRectMovementType : int {
		// No clamping, no rebound — the content can be dragged anywhere.
		Unrestricted = 0,
		// Content can be dragged past bounds with a rubber-band feel; on
		// release it springs back to within bounds. Elasticity controls
		// how soft the resistance is.
		Elastic      = 1,
		// Content is hard-clamped to bounds; user can't drag past them.
		Clamped      = 2,
	};

	// Visibility rule applied to a scrollbar attached to a ScrollRect. The
	// scrollbar entity is enabled / disabled and the viewport rect is
	// optionally inset by the scrollbar's Spacing.
	enum class ScrollbarVisibility : int {
		// Scrollbar is always shown, viewport never resized.
		Permanent              = 0,
		// Scrollbar is hidden when the content fits the viewport on its
		// axis. Viewport is NOT resized — the scrollbar just disappears.
		AutoHide               = 1,
		// Scrollbar is hidden when content fits, AND when shown the
		// viewport's edge is inset by Spacing pixels so content doesn't
		// sit underneath the scrollbar.
		AutoHideAndExpandViewport = 2,
	};

	// Scroll Rect — a clipping viewport that scrolls a Content rect to
	// reveal off-screen children. Direct port of Unity's ScrollRect:
	//
	//   Content   — child rect being scrolled. Larger than Viewport on at
	//               least one axis (otherwise there's nothing to scroll).
	//               UIEventSystem rewrites Content's AnchoredPosition every
	//               frame to expose NormalizedPosition of the content.
	//   Horizontal/Vertical — gates which axes are scrollable.
	//   MovementType — Unrestricted / Elastic / Clamped (see above).
	//   Elasticity — rubber-band stiffness when MovementType==Elastic.
	//   Inertia    — when true the content keeps drifting after the user
	//               releases, slowed down by DecelerationRate per second.
	//   ScrollSensitivity — pixels of content offset per unit of mouse
	//               wheel scroll.
	//   Viewport   — optional explicit viewport rect (defaults to the
	//               ScrollRect entity itself when unset).
	//   HorizontalScrollbar / VerticalScrollbar — optional Scrollbar
	//               entities. Their Value reflects (and is reflected by)
	//               NormalizedPosition.
	//
	// NormalizedPosition.x in [0, 1] = (0 left edge, 1 right edge).
	// NormalizedPosition.y in [0, 1] = (0 bottom edge, 1 top edge).
	// Outside [0, 1] is allowed when MovementType==Elastic or Unrestricted.
	struct ScrollRectComponent {
		EntityHandle Content = entt::null;
		EntityHandle Viewport = entt::null;

		bool Horizontal = true;
		bool Vertical = true;

		ScrollRectMovementType MovementType = ScrollRectMovementType::Elastic;
		float Elasticity = 0.1f;             // rubber-band rebound rate

		bool Inertia = true;
		float DecelerationRate = 0.135f;     // per-second friction

		// Wheel scroll multiplier in inspector-friendly units: a value of
		// 5 means "default speed", 10 = double, 1 = ⅕. Internally
		// UIEventSystem divides by 100 before multiplying the per-tick
		// pixel constant, so the on-screen scroll feels reasonable while
		// the inspector shows whole numbers users can recognise. Default
		// 5 lines up with typical OS wheel pacing for ~120 px content.
		float ScrollSensitivity = 5.0f;

		// Optional scrollbar entities + their visibility rules. The
		// scrollbar entity is found via name ("Scrollbar Horizontal" /
		// "Scrollbar Vertical") when Horizontal/VerticalScrollbar is unset.
		EntityHandle HorizontalScrollbar = entt::null;
		EntityHandle VerticalScrollbar = entt::null;
		ScrollbarVisibility HorizontalScrollbarVisibility = ScrollbarVisibility::AutoHideAndExpandViewport;
		ScrollbarVisibility VerticalScrollbarVisibility   = ScrollbarVisibility::AutoHideAndExpandViewport;
		float HorizontalScrollbarSpacing = -3.0f;
		float VerticalScrollbarSpacing   = -3.0f;

		// Current normalized position. Both components in [0, 1] when
		// content is fully inside the viewport edges; outside that range
		// is allowed in Elastic / Unrestricted mode while the user drags.
		// Read by game code as the public scroll position; UIEventSystem
		// recomputes it each frame from the content's pixel offset.
		Vec2 NormalizedPosition{ 0.0f, 1.0f };

		// Set on the frame NormalizedPosition changed by drag, wheel,
		// scrollbar drag, or programmatic write. Cleared at the start of
		// the next tick.
		bool ValueChangedThisFrame = false;
		Vec2 LastObservedNormalizedPosition{ 0.0f, 1.0f };
		bool ValueObserved = false;

		// Transient drag + inertia state. Owned by UIEventSystem.
		bool IsDragging = false;
		Vec2 PressMouseUi{ 0.0f, 0.0f };
		Vec2 PressContentPosition{ 0.0f, 0.0f }; // Content.AnchoredPosition at press
		Vec2 Velocity{ 0.0f, 0.0f };             // pixels/second, used for inertia + elastic rebound
		Vec2 PreviousContentPosition{ 0.0f, 0.0f };
	};

}
