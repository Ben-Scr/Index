#pragma once
#include "Collections/Vec2.hpp"

namespace Index {

	struct RectTransform2DComponent {
		// AnchorMin/Max/Pivot/SizeDelta describe this rect only; only Rotation/Scale propagate down the tree.
		Vec2 AnchorMin{ 0.5f, 0.5f };
		Vec2 AnchorMax{ 0.5f, 0.5f };
		Vec2 Pivot    { 0.5f, 0.5f };
		Vec2 AnchoredPosition{ 0.0f, 0.0f };
		Vec2 SizeDelta{ 100.0f, 100.0f };

		float Rotation = 0.0f;          // Z-rotation in radians (world)
		Vec2 Scale{ 1.0f, 1.0f };       // World scale

		// Inspector/serialization targets; Rotation/Scale above are derived snapshots written by UILayoutSystem.
		float LocalRotation = 0.0f;
		Vec2 LocalScale{ 1.0f, 1.0f };

		// Transient resolved screen rect (NOT serialized) — filled by UILayoutSystem each frame.
		Vec2 ResolvedMin{ 0.0f, 0.0f };
		Vec2 ResolvedMax{ 0.0f, 0.0f };
		Vec2 ResolvedPivot{ 0.0f, 0.0f }; // World-space pivot; cached for renderer rotation.
		// False until UILayoutSystem writes the resolved fields; renderer/event system fall back to authored values.
		bool ResolvedValid = false;

		Vec2 GetAuthoredSize() const {
			return Vec2{ SizeDelta.x * LocalScale.x, SizeDelta.y * LocalScale.y };
		}

		Vec2 GetAuthoredBottomLeft() const {
			const Vec2 size = GetAuthoredSize();
			return Vec2{
				AnchoredPosition.x - size.x * Pivot.x,
				AnchoredPosition.y - size.y * Pivot.y
			};
		}

		Vec2 GetAuthoredTopRight() const {
			const Vec2 bl = GetAuthoredBottomLeft();
			const Vec2 size = GetAuthoredSize();
			return Vec2{ bl.x + size.x, bl.y + size.y };
		}

		Vec2 GetBottomLeft() const {
			return ResolvedValid ? ResolvedMin : GetAuthoredBottomLeft();
		}
		Vec2 GetTopRight() const {
			return ResolvedValid ? ResolvedMax : GetAuthoredTopRight();
		}
		Vec2 GetSize() const {
			const Vec2 bl = GetBottomLeft();
			const Vec2 tr = GetTopRight();
			return Vec2{ tr.x - bl.x, tr.y - bl.y };
		}
		Vec2 GetCenter() const {
			const Vec2 bl = GetBottomLeft();
			const Vec2 tr = GetTopRight();
			return Vec2{ (bl.x + tr.x) * 0.5f, (bl.y + tr.y) * 0.5f };
		}

		bool ContainsPoint(const Vec2& screenPoint) const {
			const Vec2 bl = GetBottomLeft();
			const Vec2 tr = GetTopRight();
			return screenPoint.x >= bl.x && screenPoint.x <= tr.x
				&& screenPoint.y >= bl.y && screenPoint.y <= tr.y;
		}
	};

}
