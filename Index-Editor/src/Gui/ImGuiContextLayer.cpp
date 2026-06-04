#include "ImGuiContextLayer.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Core/Application.hpp"
#include "Core/Assert.hpp"
#include "Core/Window.hpp"
#include "Editor/EditorPreferences.hpp"
#include "Events/IndexEvent.hpp"
#include "Graphics/Text/FontHandle.hpp"
#include "Gui/ImGuiContextSetup.hpp"
#include "Gui/ImGuiFonts.hpp"
#include "Serialization/Path.hpp"
#include "Utils/ExpressionEvaluator.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace Index {

	float ImGuiContextLayer::s_DpiScale = 1.0f;

	namespace {
		constexpr const char* k_ImGuiIniFileName = "imgui.ini";

		// MUST be consumed at the start of OnPreRender BEFORE NewFrame: mid-frame ClearIniSettings+LoadIniSettingsFromDisk corrupts the dock context, leaving every dockable window floating.
		std::string s_PendingLayoutReloadPath;

		// ── Numeric expression input ─────────────────────────────────
		// Bridges ImGui scalar text entry to the engine expression evaluator so
		// typing "5/2" or "/2" into any Drag/Slider/Input numeric field evaluates.
		// Installed once on the context (OnAttach) via the SetScalarExpressionHook
		// patch in imgui_widgets.cpp; p_data arrives holding the current value.
		double ReadScalarAsDouble(ImGuiDataType type, const void* p) {
			switch (type) {
			case ImGuiDataType_S8:     return static_cast<double>(*static_cast<const ImS8*>(p));
			case ImGuiDataType_U8:     return static_cast<double>(*static_cast<const ImU8*>(p));
			case ImGuiDataType_S16:    return static_cast<double>(*static_cast<const ImS16*>(p));
			case ImGuiDataType_U16:    return static_cast<double>(*static_cast<const ImU16*>(p));
			case ImGuiDataType_S32:    return static_cast<double>(*static_cast<const ImS32*>(p));
			case ImGuiDataType_U32:    return static_cast<double>(*static_cast<const ImU32*>(p));
			case ImGuiDataType_S64:    return static_cast<double>(*static_cast<const ImS64*>(p));
			case ImGuiDataType_U64:    return static_cast<double>(*static_cast<const ImU64*>(p));
			case ImGuiDataType_Float:  return static_cast<double>(*static_cast<const float*>(p));
			case ImGuiDataType_Double: return *static_cast<const double*>(p);
			default:                   return 0.0;
			}
		}

		double RoundClamp(double v, double lo, double hi) {
			v = std::round(v);
			return v < lo ? lo : (v > hi ? hi : v);
		}

		bool WriteScalarFromDouble(ImGuiDataType type, void* p, double v) {
			switch (type) {
			case ImGuiDataType_Float:  *static_cast<float*>(p)  = static_cast<float>(v); return true;
			case ImGuiDataType_Double: *static_cast<double*>(p) = v; return true;
			case ImGuiDataType_S8:  *static_cast<ImS8*>(p)  = static_cast<ImS8>(RoundClamp(v, INT8_MIN, INT8_MAX));    return true;
			case ImGuiDataType_U8:  *static_cast<ImU8*>(p)  = static_cast<ImU8>(RoundClamp(v, 0, UINT8_MAX));          return true;
			case ImGuiDataType_S16: *static_cast<ImS16*>(p) = static_cast<ImS16>(RoundClamp(v, INT16_MIN, INT16_MAX)); return true;
			case ImGuiDataType_U16: *static_cast<ImU16*>(p) = static_cast<ImU16>(RoundClamp(v, 0, UINT16_MAX));        return true;
			case ImGuiDataType_S32: *static_cast<ImS32*>(p) = static_cast<ImS32>(RoundClamp(v, INT32_MIN, INT32_MAX)); return true;
			case ImGuiDataType_U32: *static_cast<ImU32*>(p) = static_cast<ImU32>(RoundClamp(v, 0, UINT32_MAX));        return true;
			case ImGuiDataType_S64: {
				// INT64 range exceeds double's exact-integer range; clamp in double space
				// and guard the cast so v == 2^63 (rounded INT64_MAX) can't trigger UB.
				const double r = std::round(v);
				ImS64 out;
				if (r >= 9223372036854775808.0)       out = INT64_MAX;
				else if (r <= -9223372036854775808.0) out = INT64_MIN;
				else                                  out = static_cast<ImS64>(r);
				*static_cast<ImS64*>(p) = out;
				return true;
			}
			case ImGuiDataType_U64: {
				const double r = std::round(v);
				ImU64 out;
				if (r <= 0.0)                         out = 0;
				else if (r >= 18446744073709551616.0) out = UINT64_MAX;
				else                                  out = static_cast<ImU64>(r);
				*static_cast<ImU64*>(p) = out;
				return true;
			}
			default: return false; // Bool/String/unknown — not a numeric scalar.
			}
		}

		bool ScalarExpressionHook(const char* buf, ImGuiDataType type, void* p_data) {
			double result = 0.0;
			if (!ExpressionEvaluator::Evaluate(buf, ReadScalarAsDouble(type, p_data), result)) {
				return false;
			}
			return WriteScalarFromDouble(type, p_data, result);
		}

		// ImGui can diverge window->DockId to 0 while window->DockNode stays live (observed for overlay-layer panels); WindowSettingsHandler_WriteAll reads DockId verbatim, serialising them as floating. Sync from the live pointer before every save.
		void SyncDockIdsFromLiveNodes() {
			ImGuiContext* ctx = ImGui::GetCurrentContext();
			if (ctx == nullptr) return;
			for (int n = 0; n < ctx->Windows.Size; n++) {
				ImGuiWindow* window = ctx->Windows[n];
				if (window->DockNode != nullptr &&
					window->DockId != window->DockNode->ID) {
					window->DockId = window->DockNode->ID;
				}
			}
		}

		std::filesystem::path GetCanonicalFileIfExists(const std::filesystem::path& path) {
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec) || ec) {
				return {};
			}

			std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, ec);
			return ec ? path : canonicalPath;
		}

		std::filesystem::path FindDefaultEditorIniFile() {
			std::filesystem::path executableDir;
			std::error_code ec;

			try {
				executableDir = Path::ExecutableDir();
			} catch (...) {
				executableDir.clear();
			}

			const std::filesystem::path currentDir = std::filesystem::current_path(ec);
			const std::array<std::filesystem::path, 4> candidates = {
				currentDir / "Index-Editor" / k_ImGuiIniFileName,
				executableDir / ".." / ".." / ".." / "Index-Editor" / k_ImGuiIniFileName,
				currentDir / k_ImGuiIniFileName,
				executableDir / k_ImGuiIniFileName
			};

			for (const std::filesystem::path& candidate : candidates) {
				std::filesystem::path defaultIniFile = GetCanonicalFileIfExists(candidate);
				if (!defaultIniFile.empty()) {
					return defaultIniFile;
				}
			}

			return {};
		}

		std::filesystem::path GetEditorUserIniFilePath() {
			try {
				return std::filesystem::path(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData)) /
					"Index" / "Editor" / k_ImGuiIniFileName;
			} catch (...) {
				return std::filesystem::path(k_ImGuiIniFileName);
			}
		}

		std::string ResolveEditorIniFilePath() {
			std::filesystem::path userIniFile = GetEditorUserIniFilePath();
			std::error_code ec;

			if (!userIniFile.parent_path().empty()) {
				std::filesystem::create_directories(userIniFile.parent_path(), ec);
			}

			if (!std::filesystem::is_regular_file(userIniFile, ec)) {
				if (std::filesystem::path defaultIniFile = FindDefaultEditorIniFile(); !defaultIniFile.empty()) {
					ec.clear();
					std::filesystem::copy_file(defaultIniFile, userIniFile, std::filesystem::copy_options::none, ec);
				}
			}

			return userIniFile.make_preferred().string();
		}

		std::filesystem::path GetLayoutPresetsDirectory() {
			try {
				return std::filesystem::path(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData))
					/ "Index" / "Editor" / "Layouts";
			} catch (...) {
				return std::filesystem::path("Layouts");
			}
		}

		std::filesystem::path GetLayoutPresetPath(const std::string& name) {
			return GetLayoutPresetsDirectory() / (name + ".ini");
		}
	}

	void ImGuiContextLayer::OnAttach(Application& app) {
		if (m_IsInitialized) {
			return;
		}

		Window* window = app.GetWindow();
		IDX_ASSERT(window != nullptr, IndexErrorCode::InvalidHandle, "ImGuiContextLayer requires an active Window");
		GLFWwindow* glfwWindow = window->GetGLFWWindow();
		IDX_ASSERT(glfwWindow != nullptr, IndexErrorCode::InvalidHandle, "Window has no GLFW handle");

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		// Numeric fields accept arithmetic ("5/2", "/2", …) via this evaluator hook.
		ImGui::SetScalarExpressionHook(&ScalarExpressionHook);

		EditorRuntime::ImGuiContextSetup::PublishImGuiContextToPackages();

		ImGuiIO& io = ImGui::GetIO();
		m_IniFilePath = ResolveEditorIniFilePath();
		io.IniFilename = m_IniFilePath.c_str();
		// 1s: small enough that a hard quit loses minimal layout; large enough to avoid disk thrash.
		io.IniSavingRate = 1.0f;
		// Docking/viewport flags MUST precede LoadIniSettingsFromDisk: the [Docking][Data] and [Viewport] sections only parse through handlers registered by these flags; loading first silently destroys the persisted layout.
		EditorRuntime::ImGuiContextSetup::ApplyCommonIoConfigFlags(io);
		std::error_code loadEc;
		const bool iniFileExists = std::filesystem::is_regular_file(m_IniFilePath, loadEc);
		const std::uintmax_t iniFileSize = iniFileExists
			? std::filesystem::file_size(m_IniFilePath, loadEc)
			: 0u;
		if (iniFileExists) {
			ImGui::LoadIniSettingsFromDisk(m_IniFilePath.c_str());
		}
		IDX_CORE_INFO_TAG("ImGui",
			"Editor layout file: {} ({}, {} bytes)",
			m_IniFilePath,
			iniFileExists ? "loaded" : "missing — defaults will be used",
			iniFileSize);

		const float dpiScale = EditorRuntime::ImGuiContextSetup::CaptureDpiScale(glfwWindow);
		s_DpiScale = dpiScale;

		// EditorPreferences::Load hasn't run yet (ImGuiEditorLayer::OnAttach is later); font size is finalised by ApplyEditorFontSize once prefs load.
		{
			const uint64_t fontId = EditorPreferences::GetEditorFontAssetId();
			bool loadedCustom = false;
			if (fontId != 0 && fontId != k_DefaultFontAssetId) {
				const std::string path = AssetRegistry::ResolvePath(fontId);
				if (!path.empty() && std::filesystem::exists(path)) {
					const float fontSize = k_IndexImGuiFontSize * dpiScale;
					ImFontConfig fontCfg;
					fontCfg.SizePixels = fontSize;
					fontCfg.PixelSnapH = true;
					if (io.Fonts->AddFontFromFileTTF(
							path.c_str(), fontSize, &fontCfg, io.Fonts->GetGlyphRangesDefault())) {
						loadedCustom = true;
					} else {
						IDX_CORE_WARN_TAG("ImGui",
							"Editor font '{}' failed to load (unsupported or corrupt); falling back to default.",
							path);
					}
				} else {
					IDX_CORE_WARN_TAG("ImGui",
						"Editor font asset id {} could not be resolved; falling back to default.",
						fontId);
				}
			}
			if (!loadedCustom) {
				LoadIndexImGuiFont(io, dpiScale);
			}
		}

		EditorRuntime::ImGuiContextSetup::InitBackends(glfwWindow);

		ApplyIndexTheme();
		// Must run after the theme — ScaleAllSizes is multiplicative on the
		// current style values.
		ImGui::GetStyle().ScaleAllSizes(dpiScale);

		m_IsInitialized = true;
	}

	void ImGuiContextLayer::OnDetach(Application& app) {
		(void)app;
		if (!m_IsInitialized) {
			return;
		}

		// Explicit flush before DestroyContext: the timer-driven auto-save may not have fired yet.
		const ImGuiIO& io = ImGui::GetIO();
		if (io.IniFilename != nullptr && *io.IniFilename != '\0') {
			SyncDockIdsFromLiveNodes();
			ImGui::SaveIniSettingsToDisk(io.IniFilename);
			std::error_code ec;
			const bool exists = std::filesystem::is_regular_file(io.IniFilename, ec);
			IDX_CORE_INFO_TAG("ImGui", "Saved editor layout to {} ({})",
				io.IniFilename,
				exists ? "ok" : "WRITE FAILED — path not writable?");
		}

		EditorRuntime::ImGuiContextSetup::ShutdownBackends();
		m_IniFilePath.clear();

		m_IsInitialized = false;
	}

	void ImGuiContextLayer::OnPreRender(Application& app) {
		(void)app;
		if (!m_IsInitialized) {
			return;
		}

		if (!s_PendingLayoutReloadPath.empty()) {
			const std::string path = std::move(s_PendingLayoutReloadPath);
			s_PendingLayoutReloadPath.clear();
			ImGui::ClearIniSettings();
			ImGui::LoadIniSettingsFromDisk(path.c_str());
			ImGui::GetIO().WantSaveIniSettings = false;
			m_LastSaveTime = std::chrono::steady_clock::now();
			IDX_CORE_INFO_TAG("ImGui",
				"Applied deferred layout reload from '{}'.", path);
		}

		EditorRuntime::ImGuiContextSetup::NewFrame();
	}

	void ImGuiContextLayer::OnPostRender(Application& app) {
		(void)app;
		if (!m_IsInitialized) {
			return;
		}
		EditorRuntime::ImGuiContextSetup::RenderAndPresent();

		FlushSettingsIfDirtyOrPeriodic();
	}

	void ImGuiContextLayer::ApplyEditorFontSize() {
		// _NextFrameFontSizeBase: documented mid-frame hack for FontSizeBase changes (see ImGui demo).
		ImGuiContext* ctx = ImGui::GetCurrentContext();
		if (ctx == nullptr) return;
		const float zoom = static_cast<float>(EditorPreferences::GetEditorFontZoomPercent()) / 100.0f;
		const float scaled = k_IndexImGuiFontSize * zoom * s_DpiScale;
		ImGuiStyle& style = ImGui::GetStyle();
		style.FontSizeBase = scaled;
		style._NextFrameFontSizeBase = scaled;
	}

	void ImGuiContextLayer::FlushSettingsIfDirtyOrPeriodic() {
		constexpr float k_PeriodicSaveSeconds = 5.0f;

		if (!m_IsInitialized) return;
		ImGuiIO& io = ImGui::GetIO();
		if (io.IniFilename == nullptr || *io.IniFilename == '\0') return;

		const auto now = std::chrono::steady_clock::now();
		const float secondsSinceSave = std::chrono::duration<float>(
			now - m_LastSaveTime).count();

		const bool shouldSave = io.WantSaveIniSettings
			|| secondsSinceSave >= k_PeriodicSaveSeconds;
		if (!shouldSave) return;

		SyncDockIdsFromLiveNodes();
		ImGui::SaveIniSettingsToDisk(io.IniFilename);
		io.WantSaveIniSettings = false;
		m_LastSaveTime = now;
	}

	void ImGuiContextLayer::OnEvent(Application& app, IndexEvent& event) {
		(void)app;
		if (!m_IsInitialized) return;
		const EventType type = event.GetEventType();
		if (type == EventType::WindowLostFocus || type == EventType::WindowClose) {
			ImGuiIO& io = ImGui::GetIO();
			if (io.IniFilename != nullptr && *io.IniFilename != '\0') {
				SyncDockIdsFromLiveNodes();
				ImGui::SaveIniSettingsToDisk(io.IniFilename);
				io.WantSaveIniSettings = false;
				m_LastSaveTime = std::chrono::steady_clock::now();
			}
		}
	}

	bool ImGuiContextLayer::IsValidLayoutPresetName(const std::string& name) {
		if (name.empty() || name.size() > 64) return false;
		if (name == "." || name == "..") return false;
		for (char c : name) {
			if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
				c == '"' || c == '<' || c == '>' || c == '|') {
				return false;
			}
			// Reject control chars; they're never typed deliberately and
			// produce filenames that can't be displayed in the menu.
			if (static_cast<unsigned char>(c) < 0x20) return false;
		}
		// Windows silently strips a trailing dot/space on save; refuse up front so ListLayoutPresets finds the file.
		if (name.back() == ' ' || name.back() == '.') return false;
		return true;
	}

	std::vector<std::string> ImGuiContextLayer::ListLayoutPresets() {
		std::vector<std::string> result;
		const std::filesystem::path dir = GetLayoutPresetsDirectory();
		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec)) {
			return result;
		}
		for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
			const std::filesystem::directory_entry& entry = *it;
			std::error_code ec2;
			if (!entry.is_regular_file(ec2) || ec2) continue;
			const std::filesystem::path& p = entry.path();
			if (p.extension() != ".ini") continue;
			result.push_back(p.stem().string());
		}
		std::sort(result.begin(), result.end(), [](const std::string& a, const std::string& b) {
			return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
				[](char x, char y) { return std::tolower((unsigned char)x) < std::tolower((unsigned char)y); });
		});
		return result;
	}

	bool ImGuiContextLayer::SaveLayoutPreset(const std::string& name) {
		if (!IsValidLayoutPresetName(name)) {
			IDX_CORE_WARN_TAG("ImGui",
				"SaveLayoutPreset: rejected invalid preset name '{}'.", name);
			return false;
		}
		const std::filesystem::path dir = GetLayoutPresetsDirectory();
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		if (ec) {
			IDX_CORE_WARN_TAG("ImGui",
				"SaveLayoutPreset: failed to create presets directory '{}': {}",
				dir.string(), ec.message());
			return false;
		}
		const std::filesystem::path presetPath = GetLayoutPresetPath(name);
		ImGui::SaveIniSettingsToDisk(presetPath.string().c_str());
		ec.clear();
		const bool exists = std::filesystem::is_regular_file(presetPath, ec);
		if (!exists) {
			IDX_CORE_WARN_TAG("ImGui",
				"SaveLayoutPreset: write failed (path not writable?): '{}'",
				presetPath.string());
			return false;
		}
		IDX_CORE_INFO_TAG("ImGui", "Saved layout preset '{}' → {}",
			name, presetPath.string());
		return true;
	}

	bool ImGuiContextLayer::LoadLayoutPreset(const std::string& name) {
		if (!IsValidLayoutPresetName(name)) {
			IDX_CORE_WARN_TAG("ImGui",
				"LoadLayoutPreset: rejected invalid preset name '{}'.", name);
			return false;
		}
		const std::filesystem::path presetPath = GetLayoutPresetPath(name);
		std::error_code ec;
		if (!std::filesystem::is_regular_file(presetPath, ec)) {
			IDX_CORE_WARN_TAG("ImGui",
				"LoadLayoutPreset: preset file not found at '{}'.",
				presetPath.string());
			return false;
		}
		ImGuiIO& io = ImGui::GetIO();
		if (io.IniFilename == nullptr || *io.IniFilename == '\0') {
			IDX_CORE_WARN_TAG("ImGui",
				"LoadLayoutPreset: no IniFilename set; ignoring.");
			return false;
		}
		// Write to the user ini immediately (survives a hard quit), then defer the in-context reload to start-of-next-frame (see s_PendingLayoutReloadPath).
		std::filesystem::copy_file(presetPath, io.IniFilename,
			std::filesystem::copy_options::overwrite_existing, ec);
		if (ec) {
			IDX_CORE_WARN_TAG("ImGui",
				"LoadLayoutPreset: failed to copy preset to user ini: {}",
				ec.message());
			return false;
		}
		s_PendingLayoutReloadPath = io.IniFilename;
		IDX_CORE_INFO_TAG("ImGui",
			"Queued layout preset '{}' for reload from {}",
			name, presetPath.string());
		return true;
	}

	bool ImGuiContextLayer::DeleteLayoutPreset(const std::string& name) {
		if (!IsValidLayoutPresetName(name)) {
			IDX_CORE_WARN_TAG("ImGui",
				"DeleteLayoutPreset: rejected invalid preset name '{}'.", name);
			return false;
		}
		const std::filesystem::path presetPath = GetLayoutPresetPath(name);
		std::error_code ec;
		const bool removed = std::filesystem::remove(presetPath, ec) && !ec;
		if (!removed) {
			IDX_CORE_WARN_TAG("ImGui",
				"DeleteLayoutPreset: remove failed for '{}': {}",
				presetPath.string(), ec ? ec.message() : "file did not exist");
			return false;
		}
		IDX_CORE_INFO_TAG("ImGui", "Deleted layout preset '{}' ({})",
			name, presetPath.string());
		return true;
	}

	void ImGuiContextLayer::ResetLayoutToBundledDefault() {
		ImGuiIO& io = ImGui::GetIO();
		if (io.IniFilename == nullptr || *io.IniFilename == '\0') {
			IDX_CORE_WARN_TAG("ImGui",
				"Reset Layout requested but no IniFilename is set; ignoring.");
			return;
		}

		std::filesystem::path defaultIniFile = FindDefaultEditorIniFile();
		if (defaultIniFile.empty()) {
			IDX_CORE_WARN_TAG("ImGui",
				"Reset Layout: bundled default imgui.ini not found in any "
				"candidate path. Layout left unchanged.");
			return;
		}

		std::error_code ec;
		std::filesystem::copy_file(defaultIniFile, io.IniFilename,
			std::filesystem::copy_options::overwrite_existing, ec);
		if (ec) {
			IDX_CORE_WARN_TAG("ImGui",
				"Reset Layout: failed to copy bundled default to user ini: {}",
				ec.message());
			return;
		}
		s_PendingLayoutReloadPath = io.IniFilename;

		IDX_CORE_INFO_TAG("ImGui",
			"Queued reset of editor layout from bundled default '{}' → '{}'",
			defaultIniFile.string(),
			io.IniFilename);
	}

	void ImGuiContextLayer::ApplyIndexTheme() {
		ImGuiStyle& style = ImGui::GetStyle();

		// ── Sizing & Rounding ───────────────────────────────────────
		style.WindowPadding     = ImVec2(10, 10);
		style.FramePadding      = ImVec2(6, 4);
		style.CellPadding       = ImVec2(4, 3);
		style.ItemSpacing       = ImVec2(8, 5);
		style.ItemInnerSpacing  = ImVec2(5, 4);
		style.IndentSpacing     = 16.0f;
		style.ScrollbarSize     = 13.0f;
		style.GrabMinSize       = 8.0f;

		style.WindowRounding    = 4.0f;
		style.ChildRounding     = 3.0f;
		style.FrameRounding     = 3.0f;
		style.PopupRounding     = 3.0f;
		style.ScrollbarRounding = 6.0f;
		style.GrabRounding      = 2.0f;
		style.TabRounding       = 3.0f;

		style.WindowBorderSize  = 1.0f;
		style.FrameBorderSize   = 0.0f;
		style.PopupBorderSize   = 1.0f;
		style.TabBorderSize     = 0.0f;

		style.WindowMenuButtonPosition = ImGuiDir_None;
		style.SeparatorTextBorderSize  = 2.0f;

		// Separate from sizing: EditorPreferences re-applies just the palette on theme switch without stomping DPI-scaled sizing.
		ApplyIndexThemeColors();
	}

	void ImGuiContextLayer::ApplyIndexThemeColors() {
		ImGuiStyle& style = ImGui::GetStyle();
		ImVec4* c = style.Colors;

		const ImVec4 bg        = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
		const ImVec4 bgChild   = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
		const ImVec4 bgPopup   = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
		const ImVec4 surface   = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
		const ImVec4 surfaceHi = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
		const ImVec4 surfaceAct= ImVec4(0.28f, 0.28f, 0.33f, 1.00f);
		const ImVec4 border    = ImVec4(0.24f, 0.24f, 0.28f, 0.65f);
		const ImVec4 accent    = ImVec4(0.33f, 0.53f, 0.84f, 1.00f);
		const ImVec4 neutralMark = ImVec4(0.725f, 0.725f, 0.725f, 1.00f);
		const ImVec4 text      = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
		const ImVec4 textDim   = ImVec4(0.50f, 0.50f, 0.54f, 1.00f);

		c[ImGuiCol_WindowBg]             = bg;
		c[ImGuiCol_ChildBg]              = bgChild;
		c[ImGuiCol_PopupBg]              = bgPopup;

		c[ImGuiCol_Border]               = border;
		c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);

		c[ImGuiCol_Text]                 = text;
		c[ImGuiCol_TextDisabled]         = textDim;
		c[ImGuiCol_TextSelectedBg]       = ImVec4(accent.x, accent.y, accent.z, 0.35f);

		c[ImGuiCol_FrameBg]              = surface;
		c[ImGuiCol_FrameBgHovered]       = surfaceHi;
		c[ImGuiCol_FrameBgActive]        = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);

		c[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
		c[ImGuiCol_TitleBgActive]        = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
		c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.10f, 0.10f, 0.12f, 0.75f);

		c[ImGuiCol_MenuBarBg]            = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);

		c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.10f, 0.12f, 0.53f);
		c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
		c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.35f, 0.40f, 1.00f);
		c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.42f, 0.42f, 0.48f, 1.00f);

		c[ImGuiCol_Button]               = surface;
		c[ImGuiCol_ButtonHovered]        = surfaceHi;
		c[ImGuiCol_ButtonActive]         = surfaceAct;

		c[ImGuiCol_Header]               = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
		c[ImGuiCol_HeaderHovered]        = surfaceHi;
		c[ImGuiCol_HeaderActive]         = surfaceAct;

		c[ImGuiCol_Separator]            = border;
		c[ImGuiCol_SeparatorHovered]     = ImVec4(0.36f, 0.36f, 0.42f, 1.00f);
		c[ImGuiCol_SeparatorActive]      = ImVec4(0.44f, 0.44f, 0.50f, 1.00f);

		c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ResizeGripHovered]    = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ResizeGripActive]     = ImVec4(0, 0, 0, 0);

		c[ImGuiCol_Tab]                  = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
		c[ImGuiCol_TabHovered]           = surfaceHi;
		c[ImGuiCol_TabSelected]          = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
		c[ImGuiCol_TabSelectedOverline]  = neutralMark;
		c[ImGuiCol_TabDimmed]            = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
		c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.17f, 0.17f, 0.20f, 1.00f);

		c[ImGuiCol_DockingPreview]       = ImVec4(accent.x, accent.y, accent.z, 0.40f);
		c[ImGuiCol_DockingEmptyBg]       = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);

		c[ImGuiCol_CheckMark]            = neutralMark;
		c[ImGuiCol_SliderGrab]           = ImVec4(0.40f, 0.40f, 0.46f, 1.00f);
		c[ImGuiCol_SliderGrabActive]     = ImVec4(0.50f, 0.50f, 0.56f, 1.00f);

		c[ImGuiCol_DragDropTarget]       = ImVec4(accent.x, accent.y, accent.z, 0.70f);

		c[ImGuiCol_NavCursor]            = accent;

		c[ImGuiCol_TableHeaderBg]        = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
		c[ImGuiCol_TableBorderStrong]    = border;
		c[ImGuiCol_TableBorderLight]     = ImVec4(border.x, border.y, border.z, 0.40f);
		c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_TableRowBgAlt]        = ImVec4(1, 1, 1, 0.03f);

		c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0, 0, 0, 0.55f);
	}

}
