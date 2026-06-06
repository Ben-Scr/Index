#include "Editor/EditorPreferences.hpp"

#include "Core/Log.hpp"
#include "Gui/ImGuiContextLayer.hpp"
#include "Gui/ImGuiFonts.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/SpecialFolder.hpp"
#include "Scripting/ScriptSystem.hpp"

#include <imgui.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#endif

namespace Index {
	namespace {
		// All state lives in the .cpp so the header stays light. The
		// class is a static facade — there's only ever one EditorPreferences
		// per editor process.
		struct State {
			EditorThemeMode Theme = EditorThemeMode::SystemDefault;
			ImVec4 CustomColors[ImGuiCol_COUNT]{};
			bool CustomColorsSeeded = false;

			uint64_t EditorFontAssetId = k_DefaultFontAssetId;
			int EditorFontZoomPercent = EditorPreferences::k_DefaultEditorFontZoomPercent;

			bool RunInBackground = true;
			bool ShowFileExtensions = false;
			bool AutoRecompileScripts = true;
			bool RecompileScriptsOnPlay = false;
			bool ExitPlayModeOnRecompilation = true;
			bool AutoSaveScenes = false;
			float AutoSaveIntervalSeconds = 120.0f;
			bool AutoSavePrefabs = true;
			bool ConfirmOnDelete = true;

			bool GridSnapEnabled = false;
			float GridSizeX = EditorPreferences::k_DefaultGridSize;
			float GridSizeY = EditorPreferences::k_DefaultGridSize;
			bool GridSnapLinkXY = true;

			bool Loaded = false;
			bool FreshlyCreated = false;
			bool HasAppliedTheme = false;
			EditorThemeMode LastAppliedTheme = EditorThemeMode::SystemDefault;
		};

		State& S() {
			static State s;
			return s;
		}

		std::filesystem::path PrefsPath() {
			try {
				return std::filesystem::path(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData))
					/ "Index" / "Editor" / "EditorPreferences.json";
			} catch (...) {
				return std::filesystem::path("EditorPreferences.json");
			}
		}

		const char* ThemeModeToString(EditorThemeMode mode) {
			switch (mode) {
				case EditorThemeMode::Dark:          return "Dark";
				case EditorThemeMode::Light:         return "Light";
				case EditorThemeMode::SystemDefault: return "SystemDefault";
				case EditorThemeMode::Custom:        return "Custom";
			}
			return "SystemDefault";
		}

		EditorThemeMode ThemeModeFromString(std::string_view value) {
			if (value == "Auto")          return EditorThemeMode::SystemDefault;
			if (value == "Light")         return EditorThemeMode::Light;
			if (value == "SystemDefault") return EditorThemeMode::SystemDefault;
			if (value == "Custom")        return EditorThemeMode::Custom;
			return EditorThemeMode::SystemDefault;
		}

		int NormalizeFontZoomPercent(int percent) {
			if (percent < EditorPreferences::k_MinEditorFontZoomPercent) {
				percent = EditorPreferences::k_MinEditorFontZoomPercent;
			}
			if (percent > EditorPreferences::k_MaxEditorFontZoomPercent) {
				percent = EditorPreferences::k_MaxEditorFontZoomPercent;
			}

			const int step = EditorPreferences::k_EditorFontZoomStepPercent;
			return ((percent + step / 2) / step) * step;
		}

#ifdef _WIN32
		bool IsSystemDarkMode_Win32() {
			DWORD value = 0;
			DWORD size = sizeof(value);
			LSTATUS status = RegGetValueW(
				HKEY_CURRENT_USER,
				L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
				L"AppsUseLightTheme",
				RRF_RT_REG_DWORD,
				nullptr,
				&value,
				&size);
			if (status != ERROR_SUCCESS) {
				return true; // default to dark when registry says nothing
			}
			return value == 0;
		}
#else
		bool IsSystemDarkMode_Win32() { return true; }
#endif

		EditorThemeMode ResolveTheme(EditorThemeMode mode) {
			if (mode == EditorThemeMode::SystemDefault) {
				return IsSystemDarkMode_Win32() ? EditorThemeMode::Dark : EditorThemeMode::Light;
			}
			return mode;
		}

		void MarkThemeApplied(EditorThemeMode resolved) {
			S().HasAppliedTheme = true;
			S().LastAppliedTheme = resolved;
		}

		float Luminance(const ImVec4& color) {
			return 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
		}

		Color ToColor(const ImVec4& color) {
			return Color(color.x, color.y, color.z, color.w);
		}

		void ApplyNativeTitlebarFromStyle() {
			Window* window = Application::GetWindow();
			if (!window) return;

			const ImGuiStyle& style = ImGui::GetStyle();
			const bool dark = Luminance(style.Colors[ImGuiCol_WindowBg]) < 0.5f;
			window->SetTitlebarDarkMode(dark);
			window->SetTitlebarActiveColor(Color{ 0.0f, 0.0f, 0.0f, 0.0f });
			window->SetTitlebarInactiveColor(Color{ 0.0f, 0.0f, 0.0f, 0.0f });
			window->SetTitlebarTextColor(Color{ 0.0f, 0.0f, 0.0f, 0.0f });
		}

		void SeedCustomColorsFromCurrent() {
			const ImGuiStyle& style = ImGui::GetStyle();
			std::memcpy(S().CustomColors, style.Colors, sizeof(ImVec4) * ImGuiCol_COUNT);
			S().CustomColorsSeeded = true;
		}

		Json::Value Vec4ToJson(const ImVec4& v) {
			Json::Value arr = Json::Value::MakeArray();
			arr.Append(Json::Value(static_cast<double>(v.x)));
			arr.Append(Json::Value(static_cast<double>(v.y)));
			arr.Append(Json::Value(static_cast<double>(v.z)));
			arr.Append(Json::Value(static_cast<double>(v.w)));
			return arr;
		}

		bool JsonToVec4(const Json::Value& v, ImVec4& out) {
			if (!v.IsArray()) return false;
			const Json::Value::Array& arr = v.GetArray();
			if (arr.size() != 4) return false;
			out.x = static_cast<float>(arr[0].AsDoubleOr(0.0));
			out.y = static_cast<float>(arr[1].AsDoubleOr(0.0));
			out.z = static_cast<float>(arr[2].AsDoubleOr(0.0));
			out.w = static_cast<float>(arr[3].AsDoubleOr(1.0));
			return true;
		}
	}

	void EditorPreferences::Load() {
		State& s = S();
		if (s.Loaded) return;
		s.Loaded = true;

		const std::filesystem::path path = PrefsPath();
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) {
			s.FreshlyCreated = true;
			Save();
			Application::SetRunInBackground(s.RunInBackground);
			ScriptSystem::SetAutoRecompileEnabled(s.AutoRecompileScripts);
			return;
		}

		const std::string text = File::ReadAllText(path.string());
		if (text.empty()) {
			IDX_CORE_WARN_TAG("EditorPrefs", "EditorPreferences.json was empty; using defaults.");
			return;
		}

		std::string parseError;
		Json::Value root = Json::Parse(text, &parseError);
		if (!root.IsObject()) {
			IDX_CORE_WARN_TAG("EditorPrefs",
				"Failed to parse EditorPreferences.json: {}", parseError);
			return;
		}

		if (const Json::Value* v = root.FindMember("Theme")) {
			s.Theme = ThemeModeFromString(v->AsStringOr("SystemDefault"));
		}
		if (const Json::Value* v = root.FindMember("EditorFontAssetId")) {
			// Stored as a decimal string so 64-bit ids round-trip cleanly.
			// Same convention as IndexProject::DefaultFontAssetId.
			s.EditorFontAssetId = v->IsString()
				? static_cast<uint64_t>(std::stoull(v->AsStringOr("0")))
				: v->AsUInt64Or(k_DefaultFontAssetId);
			if (s.EditorFontAssetId == 0) {
				s.EditorFontAssetId = k_DefaultFontAssetId;
			}
		}
		if (const Json::Value* v = root.FindMember("EditorFontZoomPercent")) {
			s.EditorFontZoomPercent = NormalizeFontZoomPercent(v->AsIntOr(
				EditorPreferences::k_DefaultEditorFontZoomPercent));
		}
		else if (const Json::Value* v = root.FindMember("EditorFontSize")) {
			const float size = static_cast<float>(v->AsDoubleOr(static_cast<double>(k_IndexImGuiFontSize)));
			const int percent = static_cast<int>(std::lround((size / k_IndexImGuiFontSize) * 100.0f));
			s.EditorFontZoomPercent = NormalizeFontZoomPercent(percent);
		}
		if (const Json::Value* v = root.FindMember("ShowFileExtensions")) {
			s.ShowFileExtensions = v->AsBoolOr(false);
		}
		if (const Json::Value* v = root.FindMember("RunInBackground")) {
			s.RunInBackground = v->AsBoolOr(true);
		}
		if (const Json::Value* v = root.FindMember("AutoRecompileScripts")) {
			s.AutoRecompileScripts = v->AsBoolOr(true);
		}
		if (const Json::Value* v = root.FindMember("RecompileScriptsOnPlay")) {
			s.RecompileScriptsOnPlay = v->AsBoolOr(false);
		}
		if (const Json::Value* v = root.FindMember("ExitPlayModeOnRecompilation")) {
			s.ExitPlayModeOnRecompilation = v->AsBoolOr(true);
		}
		if (const Json::Value* v = root.FindMember("AutoSaveScenes")) {
			s.AutoSaveScenes = v->AsBoolOr(false);
		}
		if (const Json::Value* v = root.FindMember("AutoSaveIntervalSeconds")) {
			s.AutoSaveIntervalSeconds = static_cast<float>(v->AsDoubleOr(120.0));
			if (s.AutoSaveIntervalSeconds < 5.0f) s.AutoSaveIntervalSeconds = 5.0f;
		}
		if (const Json::Value* v = root.FindMember("AutoSavePrefabs")) {
			s.AutoSavePrefabs = v->AsBoolOr(true);
		}
		if (const Json::Value* v = root.FindMember("ConfirmOnDelete")) {
			s.ConfirmOnDelete = v->AsBoolOr(true);
		}
		if (const Json::Value* v = root.FindMember("GridSnapEnabled")) {
			s.GridSnapEnabled = v->AsBoolOr(false);
		}
		if (const Json::Value* v = root.FindMember("GridSizeX")) {
			s.GridSizeX = static_cast<float>(v->AsDoubleOr(EditorPreferences::k_DefaultGridSize));
			if (s.GridSizeX < EditorPreferences::k_MinGridSize) s.GridSizeX = EditorPreferences::k_MinGridSize;
		}
		if (const Json::Value* v = root.FindMember("GridSizeY")) {
			s.GridSizeY = static_cast<float>(v->AsDoubleOr(EditorPreferences::k_DefaultGridSize));
			if (s.GridSizeY < EditorPreferences::k_MinGridSize) s.GridSizeY = EditorPreferences::k_MinGridSize;
		}
		if (const Json::Value* v = root.FindMember("GridSnapLinkXY")) {
			s.GridSnapLinkXY = v->AsBoolOr(true);
		}

		if (const Json::Value* v = root.FindMember("CustomColors"); v && v->IsObject()) {
			ImGuiStyle tempStyle;
			ImGui::StyleColorsDark(&tempStyle);
			std::memcpy(s.CustomColors, tempStyle.Colors, sizeof(ImVec4) * ImGuiCol_COUNT);
			for (int i = 0; i < ImGuiCol_COUNT; i++) {
				const char* name = ImGui::GetStyleColorName(i);
				if (!name) continue;
				if (const Json::Value* entry = v->FindMember(name)) {
					ImVec4 parsed;
					if (JsonToVec4(*entry, parsed)) {
						s.CustomColors[i] = parsed;
					}
				}
			}
			s.CustomColorsSeeded = true;
		}

		ImGuiContextLayer::ApplyEditorFontSize();
		Application::SetRunInBackground(s.RunInBackground);
		ScriptSystem::SetAutoRecompileEnabled(s.AutoRecompileScripts);
	}

	bool EditorPreferences::WasFreshlyCreated() {
		return S().FreshlyCreated;
	}

	void EditorPreferences::Save() {
		const State& s = S();
		const std::filesystem::path path = PrefsPath();
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);

		Json::Value root = Json::Value::MakeObject();
		root.AddMember("Theme", Json::Value(std::string(ThemeModeToString(s.Theme))));
		root.AddMember("EditorFontAssetId", Json::Value(std::to_string(s.EditorFontAssetId)));
		root.AddMember("EditorFontZoomPercent", Json::Value(s.EditorFontZoomPercent));
		root.AddMember("RunInBackground", Json::Value(s.RunInBackground));
		root.AddMember("ShowFileExtensions", Json::Value(s.ShowFileExtensions));
		root.AddMember("AutoRecompileScripts", Json::Value(s.AutoRecompileScripts));
		root.AddMember("RecompileScriptsOnPlay", Json::Value(s.RecompileScriptsOnPlay));
		root.AddMember("ExitPlayModeOnRecompilation", Json::Value(s.ExitPlayModeOnRecompilation));
		root.AddMember("AutoSaveScenes", Json::Value(s.AutoSaveScenes));
		root.AddMember("AutoSaveIntervalSeconds", Json::Value(static_cast<double>(s.AutoSaveIntervalSeconds)));
		root.AddMember("AutoSavePrefabs", Json::Value(s.AutoSavePrefabs));
		root.AddMember("ConfirmOnDelete", Json::Value(s.ConfirmOnDelete));
		root.AddMember("GridSnapEnabled", Json::Value(s.GridSnapEnabled));
		root.AddMember("GridSizeX", Json::Value(static_cast<double>(s.GridSizeX)));
		root.AddMember("GridSizeY", Json::Value(static_cast<double>(s.GridSizeY)));
		root.AddMember("GridSnapLinkXY", Json::Value(s.GridSnapLinkXY));

		// Always serialize CustomColors once seeded — the user can return
		// to the saved set after experimenting with Dark/Light/System.
		if (s.CustomColorsSeeded) {
			Json::Value colors = Json::Value::MakeObject();
			for (int i = 0; i < ImGuiCol_COUNT; i++) {
				const char* name = ImGui::GetStyleColorName(i);
				if (!name) continue;
				colors.AddMember(std::string(name), Vec4ToJson(s.CustomColors[i]));
			}
			root.AddMember("CustomColors", std::move(colors));
		}

		const std::string text = Json::Stringify(root, /*pretty*/ true);
		if (!File::WriteAllText(path.string(), text)) {
			IDX_CORE_WARN_TAG("EditorPrefs",
				"Failed to write EditorPreferences.json to '{}'.", path.string());
		}
	}

	void EditorPreferences::ApplyTheme() {
		const EditorThemeMode resolved = ResolveTheme(S().Theme);

		switch (resolved) {
			case EditorThemeMode::Light:
				ImGui::StyleColorsLight();
				MarkThemeApplied(resolved);
				ApplyNativeTitlebarFromStyle();
				return;
			case EditorThemeMode::Dark:
			case EditorThemeMode::SystemDefault: // already resolved above
				ImGuiContextLayer::ApplyIndexThemeColors();
				MarkThemeApplied(resolved);
				ApplyNativeTitlebarFromStyle();
				return;
			case EditorThemeMode::Custom: {
				if (!S().CustomColorsSeeded) {
					SeedCustomColorsFromCurrent();
				}
				ImGuiStyle& style = ImGui::GetStyle();
				std::memcpy(style.Colors, S().CustomColors, sizeof(ImVec4) * ImGuiCol_COUNT);
				MarkThemeApplied(resolved);
				ApplyNativeTitlebarFromStyle();
				return;
			}
		}
	}

	void EditorPreferences::ApplySystemThemeIfNeeded() {
		State& s = S();
		if (s.Theme != EditorThemeMode::SystemDefault) {
			return;
		}

		const EditorThemeMode resolved = ResolveTheme(s.Theme);
		if (s.HasAppliedTheme && s.LastAppliedTheme == resolved) {
			return;
		}

		ApplyTheme();
	}

	EditorThemeMode EditorPreferences::GetResolvedThemeMode() {
		return ResolveTheme(S().Theme);
	}

	EditorThemeMode EditorPreferences::GetResolvedThemeMode(EditorThemeMode mode) {
		return ResolveTheme(mode);
	}

	EditorThemeMode EditorPreferences::GetThemeMode() {
		return S().Theme;
	}

	void EditorPreferences::SetThemeMode(EditorThemeMode mode) {
		if (S().Theme == mode) return;

		if (mode == EditorThemeMode::Custom && !S().CustomColorsSeeded) {
			SeedCustomColorsFromCurrent();
		}

		S().Theme = mode;
		ApplyTheme();
		Save();
	}

	ImVec4& EditorPreferences::CustomColor(int imGuiColIdx) {
		assert(imGuiColIdx >= 0 && imGuiColIdx < ImGuiCol_COUNT);
		if (!S().CustomColorsSeeded) {
			SeedCustomColorsFromCurrent();
		}
		return S().CustomColors[imGuiColIdx];
	}

	void EditorPreferences::ResetCustomColorsFromCurrent() {
		SeedCustomColorsFromCurrent();
		Save();
	}

	uint64_t EditorPreferences::GetEditorFontAssetId() {
		return S().EditorFontAssetId;
	}

	void EditorPreferences::SetEditorFontAssetId(uint64_t id) {
		const uint64_t normalized = (id == 0) ? k_DefaultFontAssetId : id;
		if (S().EditorFontAssetId == normalized) return;
		S().EditorFontAssetId = normalized;
		Save();
	}

	int EditorPreferences::GetEditorFontZoomPercent() {
		return S().EditorFontZoomPercent;
	}

	void EditorPreferences::SetEditorFontZoomPercent(int percent) {
		const int normalized = NormalizeFontZoomPercent(percent);
		if (S().EditorFontZoomPercent == normalized) return;
		S().EditorFontZoomPercent = normalized;
		Save();
		ImGuiContextLayer::ApplyEditorFontSize();
	}

	bool EditorPreferences::GetRunInBackground() {
		return S().RunInBackground;
	}

	void EditorPreferences::SetRunInBackground(bool value) {
		if (S().RunInBackground == value) return;
		S().RunInBackground = value;
		Application::SetRunInBackground(value);
		Save();
	}

	bool EditorPreferences::GetShowFileExtensions() {
		return S().ShowFileExtensions;
	}

	void EditorPreferences::SetShowFileExtensions(bool value) {
		if (S().ShowFileExtensions == value) return;
		S().ShowFileExtensions = value;
		Save();
	}

	bool EditorPreferences::GetAutoRecompileScripts() {
		return S().AutoRecompileScripts;
	}

	void EditorPreferences::SetAutoRecompileScripts(bool value) {
		if (S().AutoRecompileScripts == value) return;
		S().AutoRecompileScripts = value;
		ScriptSystem::SetAutoRecompileEnabled(value);
		Save();
	}

	bool EditorPreferences::GetRecompileScriptsOnPlay() {
		return S().RecompileScriptsOnPlay;
	}

	void EditorPreferences::SetRecompileScriptsOnPlay(bool value) {
		if (S().RecompileScriptsOnPlay == value) return;
		S().RecompileScriptsOnPlay = value;
		Save();
	}

	bool EditorPreferences::GetExitPlayModeOnRecompilation() {
		return S().ExitPlayModeOnRecompilation;
	}

	void EditorPreferences::SetExitPlayModeOnRecompilation(bool value) {
		if (S().ExitPlayModeOnRecompilation == value) return;
		S().ExitPlayModeOnRecompilation = value;
		Save();
	}

	bool EditorPreferences::GetAutoSaveScenes() {
		return S().AutoSaveScenes;
	}

	void EditorPreferences::SetAutoSaveScenes(bool value) {
		if (S().AutoSaveScenes == value) return;
		S().AutoSaveScenes = value;
		Save();
	}

	float EditorPreferences::GetAutoSaveIntervalSeconds() {
		return S().AutoSaveIntervalSeconds;
	}

	void EditorPreferences::SetAutoSaveIntervalSeconds(float seconds) {
		const float clamped = (seconds < 5.0f) ? 5.0f : seconds;
		if (S().AutoSaveIntervalSeconds == clamped) return;
		S().AutoSaveIntervalSeconds = clamped;
		Save();
	}

	bool EditorPreferences::GetAutoSavePrefabs() {
		return S().AutoSavePrefabs;
	}

	void EditorPreferences::SetAutoSavePrefabs(bool value) {
		if (S().AutoSavePrefabs == value) return;
		S().AutoSavePrefabs = value;
		Save();
	}

	bool EditorPreferences::GetConfirmOnDelete() {
		return S().ConfirmOnDelete;
	}

	void EditorPreferences::SetConfirmOnDelete(bool value) {
		if (S().ConfirmOnDelete == value) return;
		S().ConfirmOnDelete = value;
		Save();
	}

	bool EditorPreferences::GetGridSnapEnabled() {
		return S().GridSnapEnabled;
	}

	void EditorPreferences::SetGridSnapEnabled(bool value) {
		if (S().GridSnapEnabled == value) return;
		S().GridSnapEnabled = value;
		Save();
	}

	float EditorPreferences::GetGridSizeX() {
		return S().GridSizeX;
	}

	void EditorPreferences::SetGridSizeX(float value) {
		const float clamped = (value < k_MinGridSize) ? k_MinGridSize : value;
		if (S().GridSizeX == clamped) return;
		S().GridSizeX = clamped;
		Save();
	}

	float EditorPreferences::GetGridSizeY() {
		return S().GridSizeY;
	}

	void EditorPreferences::SetGridSizeY(float value) {
		const float clamped = (value < k_MinGridSize) ? k_MinGridSize : value;
		if (S().GridSizeY == clamped) return;
		S().GridSizeY = clamped;
		Save();
	}

	bool EditorPreferences::GetGridSnapLinkXY() {
		return S().GridSnapLinkXY;
	}

	void EditorPreferences::SetGridSnapLinkXY(bool value) {
		if (S().GridSnapLinkXY == value) return;
		S().GridSnapLinkXY = value;
		Save();
	}

}
