#pragma once

#include "Collections/Color.hpp"
#include "Collections/Vec2.hpp"
#include "Core/Export.hpp"

#include <cstdint>


namespace Index {

	struct INDEX_API PostProcessing2DComponent {

		struct ColorGradingSettings {
			bool  Enabled     { false };
			float Exposure    { 0.0f };   // stops; 0 = neutral
			float Contrast    { 1.0f };   // 1 = neutral
			float Saturation  { 1.0f };   // 1 = neutral, 0 = greyscale
			float Temperature { 0.0f };   // -1..+1, cool..warm
			float Tint        { 0.0f };   // -1..+1, green..magenta
			Color ColorFilter { 1.0f, 1.0f, 1.0f, 1.0f };
		};

		struct VignetteSettings {
			bool  Enabled    { false };
			Color Color      { 0.0f, 0.0f, 0.0f, 1.0f };
			float Intensity  { 0.45f };   // 0..1
			float Smoothness { 0.3f };    // 0..1, edge falloff
			float Roundness  { 1.0f };    // 0..1, 1 = circle, 0 = follows aspect
			Vec2  Center     { 0.5f, 0.5f }; // screen UV
		};

		struct ChromaticAberrationSettings {
			bool  Enabled   { false };
			float Intensity { 0.25f };    // 0..1
		};

		struct GrainSettings {
			bool  Enabled   { false };
			float Intensity { 0.2f };     // 0..1
			float Size      { 1.0f };     // 0.3..3
			bool  Colored   { false };
		};

		struct BloomSettings {
			bool  Enabled   { false };
			float Threshold { 0.9f };  // luminance, >= 0
			float Intensity { 5.0f };  // 0..10
			float Scatter   { 0.0f };  // 0..1, blur spread
			int   Taps      { 80 };
			Color Tint      { 1.0f, 1.0f, 1.0f, 1.0f };
		};

		struct LensDistortionSettings {
			bool  Enabled   { false };
			float Intensity { 0.0f };     // -1..1; negative = pincushion, positive = barrel
			float Scale     { 1.0f };     // 0.5..1.5, compensates inward scaling under warp
			Vec2  Center    { 0.5f, 0.5f }; // screen UV
		};

		struct PixelatedSettings {
			bool  Enabled       { false };
			float BlockSize     { 8.0f };  // 1..64 pixels per "fat pixel" cell
			int   PaletteSteps  { 256 };   // 2..256 per-channel colour levels (256 = off)
			bool  QuantizeColor { false }; // toggles palette quantisation
		};

		struct GaussianBlurSettings {
			bool  Enabled { false };
			float Radius  { 0.0f }; // 0..1, scales blur extent
			// Per-direction Gaussian tap count. Same semantics as
			// BloomSettings::Taps — 7..500, higher = smoother but more
			// fragment work.
			int   Taps    { 50 };
		};

		ColorGradingSettings        ColorGrading{};
		VignetteSettings            Vignette{};
		ChromaticAberrationSettings ChromaticAberration{};
		GrainSettings               Grain{};
		BloomSettings               Bloom{};
		LensDistortionSettings      LensDistortion{};
		PixelatedSettings           Pixelated{};
		GaussianBlurSettings        GaussianBlur{};
	};

} // namespace Index
