#pragma once

#include "Collections/Color.hpp"
#include "Collections/Vec2.hpp"
#include "Core/Export.hpp"

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
			float Threshold { 1.0f };     // luminance, >= 0
			float Intensity { 0.5f };     // 0..10
			float Scatter   { 0.7f };     // 0..1, blur spread
			Color Tint      { 1.0f, 1.0f, 1.0f, 1.0f };
		};

		struct LensDistortionSettings {
			bool  Enabled   { false };
			float Intensity { 0.0f };     // -1..1; negative = pincushion, positive = barrel
			float Scale     { 1.0f };     // 0.5..1.5, compensates inward scaling under warp
			Vec2  Center    { 0.5f, 0.5f }; // screen UV
		};

		ColorGradingSettings        ColorGrading{};
		VignetteSettings            Vignette{};
		ChromaticAberrationSettings ChromaticAberration{};
		GrainSettings               Grain{};
		BloomSettings               Bloom{};
		LensDistortionSettings      LensDistortion{};
	};

} // namespace Index
