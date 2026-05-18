#pragma once

#include "Serialization/Path.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>

namespace Index {

	inline constexpr float k_IndexImGuiFontSize = 15.0f;

	inline ImFont* LoadIndexImGuiFont(ImGuiIO& io, float dpiScale = 1.0f,
									  float baseSize = k_IndexImGuiFontSize) {
		const float fontSize = baseSize * dpiScale;

		ImFontConfig fontCfg;
		fontCfg.SizePixels = fontSize;
		fontCfg.PixelSnapH = true;

		const std::string notoPath = Path::Combine(Path::ResolveIndexAssets("Fonts"), "GoogleSans", "GoogleSans-Regular.ttf");
		if (std::filesystem::exists(notoPath)) {
			if (ImFont* font = io.Fonts->AddFontFromFileTTF(
					notoPath.c_str(), fontSize, &fontCfg, io.Fonts->GetGlyphRangesDefault())) {
				return font;
			}
		}

		ImFontConfig fallbackCfg;
		fallbackCfg.SizePixels = fontSize;
		fallbackCfg.PixelSnapH = true;
		return io.Fonts->AddFontDefault(&fallbackCfg);
	}

}
