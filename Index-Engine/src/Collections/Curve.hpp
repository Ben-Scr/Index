#pragma once
#include "Collections/Vec2.hpp"

#include <cstddef>
#include <vector>

namespace Index {

	// Canonical editable curve shared across the engine + editor: cubic-bezier
	// keyframes with in/out tangent handles. x = parameter in [0,1], y = value.
	// Pairs with the reusable Index::ImGuiUtils::DrawCurveEditor widget so any graph
	// (particle scale-over-lifetime today, others later) reuses one type and one editor,
	// which also lets values be copy/pasted between graphs.
	struct Curve {
		struct Key {
			Vec2 Pos{ 0.0f, 1.0f };          // x in [0,1], y = value
			Vec2 InTangent{ -0.2f, 0.0f };   // handle offset from Pos, points left (x <= 0)
			Vec2 OutTangent{ 0.2f, 0.0f };   // handle offset from Pos, points right (x >= 0)
		};

		static std::vector<Key> DefaultKeys() {
			return {
				Key{ Vec2{ 0.0f, 1.0f }, Vec2{ -0.2f, 0.0f }, Vec2{ 0.2f, 0.0f } },
				Key{ Vec2{ 1.0f, 1.0f }, Vec2{ -0.2f, 0.0f }, Vec2{ 0.2f, 0.0f } },
			};
		}

		// Keys are kept sorted by Pos.x by the editor / on load.
		std::vector<Key> Keys = DefaultKeys();

		static Vec2 BezierPoint(const Vec2& p0, const Vec2& p1, const Vec2& p2, const Vec2& p3, float t) {
			const float u = 1.0f - t;
			const float w0 = u * u * u;
			const float w1 = 3.0f * u * u * t;
			const float w2 = 3.0f * u * t * t;
			const float w3 = t * t * t;
			return Vec2{ w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
						 w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y };
		}

		float Evaluate(float t01) const {
			if (Keys.empty()) return 1.0f;
			if (Keys.size() == 1 || t01 <= Keys.front().Pos.x) return Keys.front().Pos.y;
			if (t01 >= Keys.back().Pos.x) return Keys.back().Pos.y;
			for (std::size_t i = 1; i < Keys.size(); ++i) {
				if (t01 <= Keys[i].Pos.x) {
					const Key& a = Keys[i - 1];
					const Key& b = Keys[i];
					const Vec2 p0 = a.Pos;
					const Vec2 p1{ a.Pos.x + a.OutTangent.x, a.Pos.y + a.OutTangent.y };
					const Vec2 p2{ b.Pos.x + b.InTangent.x, b.Pos.y + b.InTangent.y };
					const Vec2 p3 = b.Pos;
					// x is monotonic in t (handle x is clamped in the editor): bisect for t(x).
					float lo = 0.0f, hi = 1.0f;
					for (int iter = 0; iter < 16; ++iter) {
						const float mid = 0.5f * (lo + hi);
						if (BezierPoint(p0, p1, p2, p3, mid).x < t01) lo = mid; else hi = mid;
					}
					return BezierPoint(p0, p1, p2, p3, 0.5f * (lo + hi)).y;
				}
			}
			return Keys.back().Pos.y;
		}
	};

}
