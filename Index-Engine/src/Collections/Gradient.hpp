#pragma once
#include "Collections/Color.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace Index {

	// Canonical editable colour gradient: independent RGB colour keys + alpha keys, each
	// positioned at t in [0,1] (Unity-style "Color over Lifetime" — colour and opacity are
	// authored separately so a constant colour can still fade out). Pairs with the reusable
	// Index::ImGuiUtils::DrawGradientEditor widget so any gradient (particle colour-over-
	// lifetime today, others later) shares one type + one editor, like Curve/DrawCurveEditor.
	struct Gradient {
		struct ColorKey {
			float Position{ 0.0f };               // t in [0,1]
			float R{ 1.0f }, G{ 1.0f }, B{ 1.0f };
		};
		struct AlphaKey {
			float Position{ 0.0f };               // t in [0,1]
			float Alpha{ 1.0f };
		};

		static std::vector<ColorKey> DefaultColorKeys() {
			return { ColorKey{ 0.0f, 1.0f, 1.0f, 1.0f }, ColorKey{ 1.0f, 1.0f, 1.0f, 1.0f } };
		}
		static std::vector<AlphaKey> DefaultAlphaKeys() {
			return { AlphaKey{ 0.0f, 1.0f }, AlphaKey{ 1.0f, 1.0f } };
		}

		// Both lists are kept sorted by Position by the editor and on load.
		std::vector<ColorKey> ColorKeys = DefaultColorKeys();
		std::vector<AlphaKey> AlphaKeys = DefaultAlphaKeys();

		// RGB at t, linearly interpolated between the two bracketing colour keys; clamps to
		// the endpoint colour outside the key range.
		Color EvaluateRGB(float t) const {
			if (ColorKeys.empty()) return Color(1.0f, 1.0f, 1.0f);
			const ColorKey& first = ColorKeys.front();
			if (ColorKeys.size() == 1 || t <= first.Position) return Color(first.R, first.G, first.B);
			const ColorKey& last = ColorKeys.back();
			if (t >= last.Position) return Color(last.R, last.G, last.B);
			for (std::size_t i = 1; i < ColorKeys.size(); ++i) {
				if (t <= ColorKeys[i].Position) {
					const ColorKey& a = ColorKeys[i - 1];
					const ColorKey& b = ColorKeys[i];
					const float span = b.Position - a.Position;
					const float f = span > 1e-6f ? (t - a.Position) / span : 0.0f;
					return Color(a.R + f * (b.R - a.R), a.G + f * (b.G - a.G), a.B + f * (b.B - a.B));
				}
			}
			return Color(last.R, last.G, last.B);
		}

		// Opacity at t, linearly interpolated between the two bracketing alpha keys.
		float EvaluateAlpha(float t) const {
			if (AlphaKeys.empty()) return 1.0f;
			const AlphaKey& first = AlphaKeys.front();
			if (AlphaKeys.size() == 1 || t <= first.Position) return first.Alpha;
			const AlphaKey& last = AlphaKeys.back();
			if (t >= last.Position) return last.Alpha;
			for (std::size_t i = 1; i < AlphaKeys.size(); ++i) {
				if (t <= AlphaKeys[i].Position) {
					const AlphaKey& a = AlphaKeys[i - 1];
					const AlphaKey& b = AlphaKeys[i];
					const float span = b.Position - a.Position;
					const float f = span > 1e-6f ? (t - a.Position) / span : 0.0f;
					return a.Alpha + f * (b.Alpha - a.Alpha);
				}
			}
			return last.Alpha;
		}

		// Full RGBA at t: colour from the colour keys, alpha from the alpha keys.
		Color Evaluate(float t) const {
			Color c = EvaluateRGB(t);
			c.a = EvaluateAlpha(t);
			return c;
		}
	};

}
