#pragma once

#include "Collections/Vec2.hpp"
#include "Scene/EntityHandle.hpp"

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

	struct ScrollRectComponent {
		EntityHandle Content = kNullEntity;
		EntityHandle Viewport = kNullEntity;

		bool Horizontal = true;
		bool Vertical = true;

		ScrollRectMovementType MovementType = ScrollRectMovementType::Elastic;
		float Elasticity = 0.1f;             // rubber-band rebound rate

		bool Inertia = true;
		float DecelerationRate = 0.135f;     // per-second friction

		// UIEventSystem divides by 100 internally; inspector shows whole numbers while actual multiplier stays small.
		float ScrollSensitivity = 5.0f;

		// Optional scrollbar entities + their visibility rules. The
		// scrollbar entity is found via name ("Scrollbar Horizontal" /
		// "Scrollbar Vertical") when Horizontal/VerticalScrollbar is unset.
		EntityHandle HorizontalScrollbar = kNullEntity;
		EntityHandle VerticalScrollbar = kNullEntity;
		ScrollbarVisibility HorizontalScrollbarVisibility = ScrollbarVisibility::AutoHideAndExpandViewport;
		ScrollbarVisibility VerticalScrollbarVisibility   = ScrollbarVisibility::AutoHideAndExpandViewport;
		float HorizontalScrollbarSpacing = -3.0f;
		float VerticalScrollbarSpacing   = -3.0f;

		Vec2 NormalizedPosition{ 0.0f, 1.0f };

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
