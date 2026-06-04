#pragma once
#include "Core/Export.hpp"
#include "Graphics/Text/FontHandle.hpp"

#include <cstdint>
#include <imgui.h>

namespace Index {

	// User-scoped preferences in %LOCALAPPDATA%\Index\Editor\EditorPreferences.json; NOT per-project.
	enum class EditorThemeMode : uint8_t {
		SystemDefault = 0,
		Dark,
		Light,
		Custom,
	};

	class EditorPreferences {
	public:
		// Call once during editor OnAttach BEFORE ApplyTheme(). Idempotent.
		static void Load();
		// True on first launch (prefs file was just created); lets the editor seed values from a legacy project without clobbering an existing pref set.
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
		// First switch to Custom seeds m_CustomColors from the current on-screen style.
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

		static bool GetAutoSavePrefabs();
		static void SetAutoSavePrefabs(bool value);

		// ── Editing ───────────────────────────────────────────────
		// When true (default), deleting an asset (Project panel) or an entity
		// (Entities hierarchy) raises a confirmation dialog first.
		static bool GetConfirmOnDelete();
		static void SetConfirmOnDelete(bool value);
	};

}
