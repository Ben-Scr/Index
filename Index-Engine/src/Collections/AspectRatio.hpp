#pragma once
#include <array>
#include <string_view>

namespace Index {

	struct AspectRatioPreset {
		const char* Label;
		float Aspect;
	};

	inline constexpr std::array<AspectRatioPreset, 6> k_AspectRatioPresets = {{
		{ "Free Aspect", 0.0f },
		{ "16:9",        16.0f /  9.0f },
		{ "21:9",        21.0f /  9.0f },
		{ "4:3",          4.0f /  3.0f },
		{ "3:2",          3.0f /  2.0f },
		{ "9:16",         9.0f / 16.0f }
	}};

	// Returns the aspect float for a stored label. Unknown labels resolve
	// to 0.0f (Free) so a typo or older project file degrades gracefully
	// to "no enforcement" instead of refusing to load.
	inline float AspectRatioFromLabel(std::string_view label) noexcept {
		for (const auto& preset : k_AspectRatioPresets) {
			if (label == preset.Label) return preset.Aspect;
		}
		return 0.0f;
	}

	// Returns the preset index for combo-box pre-selection. Unknown labels
	// fall back to index 0 ("Free Aspect").
	inline int AspectRatioPresetIndexFromLabel(std::string_view label) noexcept {
		for (int i = 0; i < static_cast<int>(k_AspectRatioPresets.size()); ++i) {
			if (label == k_AspectRatioPresets[i].Label) return i;
		}
		return 0;
	}

}
