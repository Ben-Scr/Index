#pragma once
#include "Collections/Vec2.hpp"

namespace Axiom {

	// 2D rectangle transform used by the UI system. Conceptually mirrors
	// Unity's RectTransform but trimmed to the bits this engine actually
	// needs:
	//
	//   AnchorMin / AnchorMax — fractional anchors inside the parent's
	//     rect, in [0, 1]. AnchorMin == AnchorMax means a point anchor
	//     (the rect floats with that fraction of the parent). When they
	//     differ on an axis the rect stretches, so size on that axis
	//     becomes parent_size_on_axis * (max - min) + SizeDelta.
	//   Pivot — fractional point inside the rect that AnchoredPosition
	//     refers to. (0.5, 0.5) keeps the rect centred on the anchor;
	//     (0, 0) puts the bottom-left at the anchor. Also the centre of
	//     rotation and scale.
	//   AnchoredPosition — offset from the anchor (or anchor mid-point
	//     when anchors differ) to the pivot, in pixel units.
	//   SizeDelta — additional size on top of the stretched anchor span.
	//     For point anchors (min == max) this *is* the size.
	//
	// The rect is laid out in centered screen-space: the origin (0, 0) is
	// the centre of the window, +X right, +Y up. Without a parent, the
	// "parent rect" is the full window viewport. Positions are pixels.
	//
	// UILayoutSystem walks the hierarchy each frame and writes the
	// resolved AABB into ResolvedMin / ResolvedMax (also centered screen
	// space). UIRenderer + UIEventSystem read those for actual drawing
	// and hit-testing — game code should treat them as read-only.
	struct RectTransform2DComponent {
		// Anchored layout (Unity-style). Defaults match a centred,
		// 100x100 rect with a centred pivot — the common "drop in a
		// button" case. AnchorMin/Max/Pivot/SizeDelta are NEVER inherited
		// from a parent RectTransform — they describe this rect's own
		// position inside its parent and its own dimensions, not values
		// to compose. Only Rotation and Scale propagate down the tree
		// (mirroring Transform2D's world/local split below).
		Vec2 AnchorMin{ 0.5f, 0.5f };
		Vec2 AnchorMax{ 0.5f, 0.5f };
		Vec2 Pivot    { 0.5f, 0.5f };
		Vec2 AnchoredPosition{ 0.0f, 0.0f };
		Vec2 SizeDelta{ 100.0f, 100.0f };

		// World-space rotation + scale. Written by UILayoutSystem each
		// frame as (parent's world rotation/scale composed with this
		// entity's Local* values), and read by the renderer / event
		// system. Mirrors the Position/Rotation/Scale fields on
		// Transform2DComponent — keep the public field names the same
		// so existing readers stay compatible.
		float Rotation = 0.0f;          // Z-rotation in radians (world)
		Vec2 Scale{ 1.0f, 1.0f };       // World scale

		// Authored local-space rotation + scale, applied around Pivot.
		// For root rects these match Rotation/Scale; for child rects
		// they describe the offset from the parent's world rotation /
		// scale. Inspector edits and JSON (de)serialization act on
		// these — Rotation/Scale above are derived snapshots.
		float LocalRotation = 0.0f;
		Vec2 LocalScale{ 1.0f, 1.0f };

		// ── Transient resolved screen rect (NOT serialized) ───────────
		// Filled by UILayoutSystem each frame. Bottom-left and top-right
		// corners of the rect in centered screen-space (origin = window
		// centre, +Y up). When no UILayoutSystem has run yet, these
		// default to the AuthoredAABB() result.
		Vec2 ResolvedMin{ 0.0f, 0.0f };
		Vec2 ResolvedMax{ 0.0f, 0.0f };
		// World-space pivot point (resolved). Cached for renderer rotation.
		Vec2 ResolvedPivot{ 0.0f, 0.0f };
		// Has UILayoutSystem written into the resolved fields this frame?
		// Renderer / event system fall back to the authored unparented
		// rect when this is false to keep newly-created entities visible
		// before the first layout pass.
		bool ResolvedValid = false;

		// ── Authored convenience (point-anchor / unparented case) ─────
		// These fall back to the authored local values, useful as the
		// initial state before the first layout tick or for components
		// that haven't been added to a scene yet.
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

		// ── Resolved (read these from gameplay code) ──────────────────
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
