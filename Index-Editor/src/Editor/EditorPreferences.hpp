#pragma once
#include "Core/Export.hpp"
#include "Graphics/Text/FontHandle.hpp"

#include <cstdint>
#include <imgui.h>

namespace Index {

	// User-scoped editor preferences. Lives in
	// %LOCALAPPDATA%\Index\Editor\EditorPreferences.json and is loaded once
	// at editor startup. Anything here is intentionally NOT per-project —
	// it follows the user, not the project file. Project-scoped settings
	// still live on IndexProject.
	enum class EditorThemeMode : uint8_t {
		SystemDefault = 0,
		Dark,
		Light,
		Custom,
	};

	class EditorPreferences {
	public:
		// Read JSON from disk (creates file with defaults if missing).
		// Call once during editor OnAttach BEFORE ApplyTheme(). Idempotent.
		static void Load();
		// True if the most recent Load() call had to create a new prefs
		// file from defaults (i.e. first launch). Used by the editor's
		// legacy-project migration: when a project loaded with pre-2026-05
		// editor-pref fields is opened on a fresh editor install, those
		// values seed EditorPreferences. On subsequent launches the user's
		// already-customised prefs win — legacy project values are ignored.
		static bool WasFreshlyCreated();
		// Persist current state to disk. Cheap synchronous write; called
		// after every mutation by the Set*-helpers below.
		static void Save();

		// Push the active theme into the ImGui style. Re-resolves
		// SystemDefault each call so a Windows theme change is honored.
		static void ApplyTheme();
		// Lightweight per-frame hook for SystemDefault. No-ops unless the
		// OS-resolved theme changed since the last applied editor theme.
		static void ApplySystemThemeIfNeeded();
		static EditorThemeMode GetResolvedThemeMode();
		static EditorThemeMode GetResolvedThemeMode(EditorThemeMode mode);

		// ── Appearance ────────────────────────────────────────────
		static EditorThemeMode GetThemeMode();
		// Setting Custom for the first time seeds m_CustomColors from the
		// style currently on screen so the user starts from "what they
		// were just looking at" rather than zeroed-out swatches.
		static void SetThemeMode(EditorThemeMode mode);
		// Mutable accessor — ColorEdit4 writes through this. Caller must
		// pass a valid ImGuiCol_ index (asserted internally).
		static ImVec4& CustomColor(int imGuiColIdx);
		// Reseeds m_CustomColors from the live ImGui style. Useful as a
		// "reset" button after the user has experimented and wants to
		// return to the current base theme's colors.
		static void ResetCustomColorsFromCurrent();

		static uint64_t GetEditorFontAssetId();
		// Persisted immediately; the new font only takes effect on next
		// editor launch (font atlas rebuild is not wired in this change).
		static void SetEditorFontAssetId(uint64_t id);

		static int GetEditorFontZoomPercent();
		// Clamped to [k_MinEditorFontZoomPercent, k_MaxEditorFontZoomPercent]
		// and snapped to k_EditorFontZoomStepPercent. Persisted immediately
		// and applied live through ImGuiContextLayer.
		static void SetEditorFontZoomPercent(int percent);

		static constexpr int k_MinEditorFontZoomPercent = 75;
		static constexpr int k_MaxEditorFontZoomPercent = 200;
		static constexpr int k_EditorFontZoomStepPercent = 25;
		static constexpr int k_DefaultEditorFontZoomPercent = 100;

		// ── Behavior (migrated from IndexProject) ─────────────────
		static bool GetRunInBackground();
		static void SetRunInBackground(bool value);

		static bool GetShowFileExtensions();
		static void SetShowFileExtensions(bool value);

		static bool GetAutoRecompileScripts();
		static void SetAutoRecompileScripts(bool value);
		static bool GetRecompileScriptsOnPlay();
		static void SetRecompileScriptsOnPlay(bool value);
		static bool GetExitPlayModeOnRecompilation();
		static void SetExitPlayModeOnRecompilation(bool value);

		static bool GetAutoSaveScenes();
		static void SetAutoSaveScenes(bool value);
		static float GetAutoSaveIntervalSeconds();
		// Clamped to >= 5.0 to match the lower bound the project-settings
		// UI enforced when these fields lived on IndexProject.
		static void SetAutoSaveIntervalSeconds(float seconds);

		// Auto-save for in-viewport prefab edit mode. Event-driven (saves on
		// widget release via ImGui::IsAnyItemActive), so unlike scene
		// auto-save there is no interval — the toggle alone gates it. On by
		// default since the in-viewport prefab edit flow has no separate
		// save-and-keep-editing affordance besides the toolbar button.
		static bool GetAutoSavePrefabs();
		static void SetAutoSavePrefabs(bool value);
	};

}
