#pragma once

#include "Core/Log.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/SpecialFolder.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>

namespace Index {

	inline constexpr float k_IndexImGuiFontSize = 15.0f;

	inline ImFont* LoadIndexImGuiFont(ImGuiIO& io, float dpiScale = 1.0f,
									  float baseSize = k_IndexImGuiFontSize) {
		const float fontSize = baseSize * dpiScale;

		// Explicit glyph ranges required: each binary links its own ImGui; the launcher bakes a static atlas so nullptr range would produce nothing.
		ImFontConfig primaryCfg;
		primaryCfg.SizePixels = fontSize;
		primaryCfg.PixelSnapH = true;

		ImFont* primary = nullptr;
		const std::string googleSansPath = Path::Combine(
			Path::ResolveIndexAssets("Fonts"), "GoogleSans-Regular.ttf");
		if (std::filesystem::exists(googleSansPath)) {
			primary = io.Fonts->AddFontFromFileTTF(
				googleSansPath.c_str(), fontSize, &primaryCfg,
				io.Fonts->GetGlyphRangesDefault());
		}

		// Merged CJK fallback. The Localization service downloads this font
		// on-demand to the user-writable location; we prefer that copy so
		// the bundle can ship without it. Fall back to the bundled IndexAssets
		// path for the dev-layout install (still useful while iterating).
		std::string userCjkPath;
		try {
			userCjkPath = (std::filesystem::path(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData))
				/ "Index" / "Fonts" / "NotoSansCJK" / "NotoSansCJK-Regular.ttc").string();
		}
		catch (...) {
			userCjkPath.clear();
		}
		const std::string bundledCjkPath = Path::Combine(
			Path::ResolveIndexAssets("Fonts"), "NotoSansCJK", "NotoSansCJK-Regular.ttc");

		std::string cjkPath;
		if (!userCjkPath.empty() && std::filesystem::exists(userCjkPath)) {
			cjkPath = userCjkPath;
		}
		else if (std::filesystem::exists(bundledCjkPath)) {
			cjkPath = bundledCjkPath;
		}

		ImFontConfig cjkCfg;
		cjkCfg.SizePixels = fontSize;
		cjkCfg.PixelSnapH = true;
		cjkCfg.MergeMode = true;
		// MergeMode requires an already-added base font; with no primary
		// ImGui reads Fonts.back() of an empty vector → 0xC0000005.
		if (primary && !cjkPath.empty()) {
			io.Fonts->AddFontFromFileTTF(
				cjkPath.c_str(), fontSize, &cjkCfg,
				io.Fonts->GetGlyphRangesJapanese());
		}
		else if (cjkPath.empty()) {
			IDX_CORE_WARN_TAG("Localization",
				"CJK font not installed; CJK characters will render as boxes. "
				"It will be downloaded the first time you select a CJK language.");
		}

		if (primary) return primary;

		ImFontConfig fallbackCfg;
		fallbackCfg.SizePixels = fontSize;
		fallbackCfg.PixelSnapH = true;
		return io.Fonts->AddFontDefault(&fallbackCfg);
	}

}
