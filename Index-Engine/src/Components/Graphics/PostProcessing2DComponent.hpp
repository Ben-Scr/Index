#pragma once

#include "Collections/Color.hpp"
#include "Collections/Vec2.hpp"
#include "Core/Export.hpp"

#include <cstdint>

// =============================================================================
// PostProcessing2DComponent — per-camera post-process settings (data only).
//
// Each effect group carries its own Enabled toggle plus parameters. The
// renderer is expected to look up this component on the camera entity and
// run the matching passes when the corresponding Enabled bit is true; until
// the renderer hookup lands the values exist only to be authored, saved,
// and loaded.
//
// POD by design — no scene back-pointer, no asset handles, no runtime
// caches — so the registry's auto-generated copyTo / emplaceFromBytes are
// correct without any overrides.
// =============================================================================

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
			float Threshold { 1.0f };  // luminance, >= 0
			float Intensity { 0.5f };  // 0..10
			float Scatter   { 0.7f };  // 0..1, blur spread
			// Per-direction Gaussian tap count. Range 7..500 — drives
			// the separable blur's loop count. 7 is fast/grainy, 21 is
			// a balanced default, 500 is "throw a GPU at it" smooth.
			// Cost scales linearly with Taps (separable blur), so going
			// from 21 -> 101 is ~5x the bloom shader cost.
			int   Taps      { 21 };
			Color Tint      { 1.0f, 1.0f, 1.0f, 1.0f };
		};

		struct LensDistortionSettings {
			bool  Enabled   { false };
			float Intensity { 0.0f };     // -1..1; negative = pincushion, positive = barrel
			float Scale     { 1.0f };     // 0.5..1.5, compensates inward scaling under warp
			Vec2  Center    { 0.5f, 0.5f }; // screen UV
		};

		// Pixelated — snaps the sampled UV to a coarse grid so the
		// scene reads as discrete pixel cells. Optional Palette
		// quantisation reduces the per-channel colour depth (8 bit/ch
		// at PaletteSteps = 256 = no quantisation; 4 = a 64-colour
		// palette; 2 = 8-colour). Combined with low BlockSize it makes
		// a "retro / 1-bit" look, with high BlockSize it makes large
		// blocky mosaics.
		struct PixelatedSettings {
			bool  Enabled       { false };
			float BlockSize     { 8.0f };  // 1..64 pixels per "fat pixel" cell
			int   PaletteSteps  { 256 };   // 2..256 per-channel colour levels (256 = off)
			bool  QuantizeColor { false }; // toggles palette quantisation
		};

		// Gaussian Blur — separable two-pass blur applied to the WHOLE
		// scene (not just bright pixels like Bloom). Runs at full
		// resolution so the result stays sharp. Useful for UI background
		// blur, frosted-glass looks, or fake depth-of-field. Distinct
		// from Bloom: this REPLACES the scene contents with the blurred
		// version, where Bloom adds a halo on top.
		struct GaussianBlurSettings {
			bool  Enabled { false };
			float Radius  { 0.5f }; // 0..1, scales blur extent
			// Per-direction Gaussian tap count. Same semantics as
			// BloomSettings::Taps — 7..500, higher = smoother but more
			// fragment work.
			int   Taps    { 21 };
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
