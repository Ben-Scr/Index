#include "EditorPreferencesPanel.hpp"

#include "Assets/AssetKind.hpp"
#include "Core/Application.hpp"
#include "Editor/EditorPreferences.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Editor/ExternalEditorInfo.hpp"
#include "Graphics/Text/FontHandle.hpp"
#include "Gui/ImGuiContextLayer.hpp"
#include "Gui/ImGuiFonts.hpp"
#include "Gui/ImGuiImplWebGPU.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Inspector/ReferencePicker.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace Index {
	namespace {
		constexpr const char* k_FontPickerKey = "EditorPrefs.EditorFont";

		uint64_t ParseUInt64String(const std::string& value) {
			if (value.empty()) return 0;
			try {
				size_t parsed = 0;
				uint64_t id = std::stoull(value, &parsed, 10);
				return parsed == value.size() ? id : 0;
			} catch (...) {
				return 0;
			}
		}

		const char* ThemeModeLabel(EditorThemeMode mode) {
			switch (mode) {
				case EditorThemeMode::Dark:          return "Dark";
				case EditorThemeMode::Light:         return "Light";
				case EditorThemeMode::SystemDefault:
					return EditorPreferences::GetResolvedThemeMode(EditorThemeMode::SystemDefault) == EditorThemeMode::Light
						? "Auto (Light)"
						: "Auto (Dark)";
				case EditorThemeMode::Custom:        return "Custom";
			}
			return "Auto (Dark)";
		}

		const char* FontZoomLabel(int percent) {
			switch (percent) {
				case 75:  return "75%";
				case 100: return "100%";
				case 125: return "125%";
				case 150: return "150%";
				case 175: return "175%";
				case 200: return "200%";
			}
			return "100%";
		}

		std::string LowerCopy(std::string value) {
			std::transform(value.begin(), value.end(), value.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}
	}

	void EditorPreferencesPanel::Initialize() {
		// Reserved for future per-panel state setup. Detection of script
		// editors is on-demand in RenderScriptingTab so a slow first-time
		// probe doesn't run at editor startup.
	}

	void EditorPreferencesPanel::Shutdown() {
		// No persistent allocations to drop.
	}

	void EditorPreferencesPanel::Render(bool* pOpen) {
		if (!pOpen || !*pOpen) {
			m_WasOpenLastFrame = false;
			return;
		}

		// First-frame open hook: refresh editor detection so the user gets
		// a current list every time they open the panel without having to
		// restart the editor after installing a new IDE.
		if (!m_WasOpenLastFrame) {
			ExternalEditor::DetectEditors();
		}
		m_WasOpenLastFrame = true;

		ImGui::SetNextWindowSize(ImVec2(640.0f, 520.0f), ImGuiCond_FirstUseEver);
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (!ImGui::Begin("Editor Preferences", pOpen)) {
			ImGui::End();
			return;
		}

		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##EditorPrefsSearch", "Search preferences...",
			m_SearchBuffer, sizeof(m_SearchBuffer));
		ImGui::Spacing();

		if (IsPreferenceSearchActive()) {
			ImGui::BeginChild("##EditorPrefsSearchResults", ImVec2(0, 0), false,
				ImGuiWindowFlags_AlwaysVerticalScrollbar);
			RenderAppearanceTab();
			RenderScriptingTab();
			RenderLayoutsTab();
			RenderBehaviorTab();
			ImGui::EndChild();
		}
		else if (ImGui::BeginTabBar("##EditorPrefsTabs", ImGuiTabBarFlags_None)) {
			// Each tab's body lives in a scrolling child so the search bar and
			// the tab bar stay pinned at the top while a long tab scrolls.
			if (ImGui::BeginTabItem("Appearance")) {
				ImGui::BeginChild("##AppearanceScroll", ImVec2(0, 0), false);
				RenderAppearanceTab();
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Scripting")) {
				ImGui::BeginChild("##ScriptingScroll", ImVec2(0, 0), false);
				RenderScriptingTab();
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Layouts")) {
				ImGui::BeginChild("##LayoutsScroll", ImVec2(0, 0), false);
				RenderLayoutsTab();
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Behavior")) {
				ImGui::BeginChild("##BehaviorScroll", ImVec2(0, 0), false);
				RenderBehaviorTab();
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		// Reference picker popup for the editor-font selector. Must run
		// inside the same ImGui::Begin scope as the button that opens it.
		ReferencePicker::RenderPopup();

		// Layout modals are owned by this panel (not the menubar) so the
		// OpenPopup / BeginPopupModal id-stack match. Render them at the
		// panel's Begin/End scope, after the tab bar.
		RenderFontRestartModal();
		RenderLayoutModals();

		ImGui::End();
	}

	bool EditorPreferencesPanel::IsPreferenceSearchActive() const {
		return m_SearchBuffer[0] != '\0';
	}

	bool EditorPreferencesPanel::PreferenceSectionVisible(const char* section, const char* keywords) const {
		if (!IsPreferenceSearchActive()) return true;
		const std::string filter = LowerCopy(m_SearchBuffer);
		std::string haystack = LowerCopy(section ? section : "");
		haystack += ' ';
		haystack += LowerCopy(keywords ? keywords : "");
		return haystack.find(filter) != std::string::npos;
	}

	void EditorPreferencesPanel::RenderAppearanceTab() {
		const bool showTheme = PreferenceSectionVisible("Theme", "appearance auto dark light custom color colors palette");
		const bool showFont = PreferenceSectionVisible("Editor Font", "appearance editor font typeface google sans size zoom");
		const bool showHierarchy = PreferenceSectionVisible("Entities Hierarchy",
			"appearance entities hierarchy outliner row size scale height bigger larger smaller");
		if (!showTheme && !showFont && !showHierarchy) return;

		// ── Theme ───────────────────────────────────────────────────
		if (showTheme) {
		ImGui::TextUnformatted("Theme");
		ImGui::Separator();

		EditorThemeMode currentMode = EditorPreferences::GetThemeMode();
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::BeginCombo("##EditorThemeCombo", ThemeModeLabel(currentMode))) {
			constexpr EditorThemeMode kModes[] = {
				EditorThemeMode::SystemDefault,
				EditorThemeMode::Dark,
				EditorThemeMode::Light,
				EditorThemeMode::Custom,
			};
			for (EditorThemeMode mode : kModes) {
				const bool selected = (mode == currentMode);
				if (ImGui::Selectable(ThemeModeLabel(mode), selected)) {
					EditorPreferences::SetThemeMode(mode);
					currentMode = mode;
				}
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("The theme of the editor ui");
		}

		ImGui::Spacing();

		// ── Custom Colors ──────────────────────────────────────────
		const bool customActive = (currentMode == EditorThemeMode::Custom);
		ImGui::BeginDisabled(!customActive);

		ImGui::TextUnformatted("Custom Colors");
		ImGui::SameLine();
		if (ImGui::Button("Reset to Default##ResetCustomColors")) {
			EditorPreferences::ResetCustomColorsToDefault();
			// Re-apply so the swatches snap back to the just-seeded values.
			EditorPreferences::ApplyTheme();
		}
		if (ImGui::IsItemHovered() && customActive) {
			ImGui::SetTooltip(
				"Resets the custom theme back to it's default");
		}

		// Scrollable region so the long ImGuiCol_ list doesn't push the
		// rest of the tab off-screen on smaller windows.
		ImGui::BeginChild("##CustomColorsScroll",
			ImVec2(0, 280.0f),
			ImGuiChildFlags_Borders,
			ImGuiWindowFlags_None);

		for (int i = 0; i < ImGuiCol_COUNT; i++) {
			const char* name = ImGui::GetStyleColorName(i);
			if (!name) continue;
			ImVec4& color = EditorPreferences::CustomColor(i);
			ImGui::PushID(i);
			if (ImGui::ColorEdit4(name, &color.x,
					ImGuiColorEditFlags_AlphaBar |
					ImGuiColorEditFlags_NoInputs)) {
				if (customActive) {
					// Push directly into the live style so the change is
					// visible mid-frame, and persist so it survives restart.
					ImGui::GetStyle().Colors[i] = color;
					EditorPreferences::Save();
				}
			}
			ImGui::PopID();
		}

		ImGui::EndChild();
		ImGui::EndDisabled();

		ImGui::Spacing();

		// ── Editor Font ────────────────────────────────────────────
		}

		if (showFont) {
		ImGui::TextUnformatted("Editor Font");
		ImGui::Separator();

		// Consume any selection raised on the previous frame.
		if (auto pending = ReferencePicker::ConsumeSelection(k_FontPickerKey); pending) {
			const uint64_t picked = ParseUInt64String(*pending);
			const uint64_t before = EditorPreferences::GetEditorFontAssetId();
			EditorPreferences::SetEditorFontAssetId(picked != 0 ? picked : k_DefaultFontAssetId);
			if (EditorPreferences::GetEditorFontAssetId() != before) {
				m_OpenFontRestartRequest = true;
			}
		}

		bool fontMissing = false;
		std::string fontSecondary;
		const uint64_t fontId = EditorPreferences::GetEditorFontAssetId();
		const std::string fontDisplay = ReferencePicker::ResolveAssetDisplay(
			fontId, AssetKind::Font, fontMissing, &fontSecondary);

		ImGui::PushID("EditorFontPicker");
		if (ImGui::Button(fontDisplay.c_str(), ImVec2(280.0f, 0.0f))) {
			ReferencePicker::OpenForFieldKey(k_FontPickerKey, "Select Editor Font",
				ReferencePicker::CollectAssetsByKind(AssetKind::Font),
				ReferencePicker::Style::Plain);
		}
		if (ImGui::IsItemHovered() && !fontSecondary.empty()) {
			ImGui::SetTooltip("%s", fontSecondary.c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset##EditorFontReset")) {
			const uint64_t before = EditorPreferences::GetEditorFontAssetId();
			EditorPreferences::SetEditorFontAssetId(k_DefaultFontAssetId);
			if (EditorPreferences::GetEditorFontAssetId() != before) {
				m_OpenFontRestartRequest = true;
			}
		}
		ImGui::PopID();
		ImGui::Spacing();

		ImGui::PushID("EditorFontZoom");
		int fontZoom = EditorPreferences::GetEditorFontZoomPercent();
		ImGui::SetNextItemWidth(280.0f);
		if (ImGui::BeginCombo("Font Size", FontZoomLabel(fontZoom))) {
			for (int percent = EditorPreferences::k_MinEditorFontZoomPercent;
				percent <= EditorPreferences::k_MaxEditorFontZoomPercent;
				percent += EditorPreferences::k_EditorFontZoomStepPercent)
			{
				const bool selected = (fontZoom == percent);
				if (ImGui::Selectable(FontZoomLabel(percent), selected)) {
					EditorPreferences::SetEditorFontZoomPercent(percent);
					fontZoom = percent;
				}
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset##EditorFontZoomReset")) {
			EditorPreferences::SetEditorFontZoomPercent(
				EditorPreferences::k_DefaultEditorFontZoomPercent);
		}
		ImGui::PopID();
		}

		// ── Entities Hierarchy ─────────────────────────────────────
		if (showHierarchy) {
		ImGui::Spacing();
		ImGui::TextUnformatted("Entities Hierarchy");
		ImGui::Separator();

		ImGui::PushID("HierarchyRowScale");
		float rowScale = EditorPreferences::GetHierarchyRowScale();
		ImGui::SetNextItemWidth(280.0f);
		if (ImGui::SliderFloat("Row Size", &rowScale,
				EditorPreferences::k_MinHierarchyRowScale,
				EditorPreferences::k_MaxHierarchyRowScale,
				"%.2fx", ImGuiSliderFlags_AlwaysClamp)) {
			EditorPreferences::SetHierarchyRowScale(rowScale);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Scales the entity rows in the Entities panel.");
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset##HierarchyRowScaleReset")) {
			EditorPreferences::SetHierarchyRowScale(
				EditorPreferences::k_DefaultHierarchyRowScale);
		}
		ImGui::PopID();
		}
	}

	void EditorPreferencesPanel::RenderFontRestartModal() {
		if (m_OpenFontRestartRequest) {
			ImGui::OpenPopup("Restart Editor?");
			m_OpenFontRestartRequest = false;
		}

		ImGuiUtils::CenterNextModal();
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::BeginPopupModal("Restart Editor?", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::TextWrapped(
				"The editor font will change after restarting the editor.\n\n"
				"Restart now?");
			ImGui::Spacing();
			if (ImGui::Button("Restart Now", ImVec2(120.0f, 0.0f))) {
				ImGui::CloseCurrentPopup();
				Application::Reload();
			}
			ImGui::SameLine();
			if (ImGui::Button("Later", ImVec2(90.0f, 0.0f))) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	void EditorPreferencesPanel::RenderScriptingTab() {
		const bool showExternalEditor = PreferenceSectionVisible("External Script Editor", "scripting script editor external ide code visual studio rider");
		const bool showCompilation = PreferenceSectionVisible("Script Compilation", "scripting recompile recompilation auto file changes play mode exit compile");
		if (!showExternalEditor && !showCompilation) return;

		if (showExternalEditor) {
		ImGui::TextUnformatted("External Script Editor");
		ImGui::Separator();

		const auto& editors = ExternalEditor::GetAvailableEditors();
		if (editors.empty()) {
			ImGui::TextDisabled("No supported editors detected on this system.");
			if (ImGui::Button("Re-detect")) {
				ExternalEditor::DetectEditors();
			}
		}
		else {

		const int selected = ExternalEditor::GetSelectedIndex();
		const char* preview = (selected >= 0 && selected < static_cast<int>(editors.size()))
			? editors[selected].DisplayName.c_str()
			: "(none)";

		ImGui::SetNextItemWidth(320.0f);
		if (ImGui::BeginCombo("##ExternalEditorCombo", preview)) {
			for (int i = 0; i < static_cast<int>(editors.size()); i++) {
				const bool isSelected = (i == selected);
				if (ImGui::Selectable(editors[i].DisplayName.c_str(), isSelected)) {
					ExternalEditor::SetSelectedIndex(i);
				}
				if (isSelected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button("Re-detect##ScriptEditorRedetect")) {
			ExternalEditor::DetectEditors();
		}
		ImGui::TextDisabled("The editor used when opening script files.");
		}
		}

		if (showCompilation) {
			if (showExternalEditor) ImGui::Spacing();
			ImGui::TextUnformatted("Script Compilation");
			ImGui::Separator();

			bool autoRecompile = EditorPreferences::GetAutoRecompileScripts();
			if (ImGui::Checkbox("Auto-recompile on file changes", &autoRecompile)) {
				EditorPreferences::SetAutoRecompileScripts(autoRecompile);
			}

			bool recompileOnPlay = EditorPreferences::GetRecompileScriptsOnPlay();
			if (ImGui::Checkbox("Recompile before Play Mode", &recompileOnPlay)) {
				EditorPreferences::SetRecompileScriptsOnPlay(recompileOnPlay);
			}

			bool exitOnRecompile = EditorPreferences::GetExitPlayModeOnRecompilation();
			if (ImGui::Checkbox("Exit Play Mode on Recompilation", &exitOnRecompile)) {
				EditorPreferences::SetExitPlayModeOnRecompilation(exitOnRecompile);
			}
		}
	}

	void EditorPreferencesPanel::RenderLayoutsTab() {
		if (!PreferenceSectionVisible("Layout Presets", "layout layouts preset save load reset delete")) return;

		ImGui::TextUnformatted("Layout Presets");
		ImGui::Separator();

		if (ImGui::Button("Save Current Layout As...")) {
			m_SaveLayoutBuffer[0] = '\0';
			m_OpenSaveLayoutRequest = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset to Default")) {
			ImGuiContextLayer::ResetLayoutToBundledDefault();
		}
		ImGui::TextDisabled("Stored under %%LOCALAPPDATA%%\\Index\\Editor\\Layouts.");

		ImGui::Spacing();

		const std::vector<std::string> presets =
			ImGuiContextLayer::ListLayoutPresets();
		if (presets.empty()) {
			ImGui::TextDisabled("No saved layout presets.");
			return;
		}

		if (ImGui::BeginTable("##LayoutPresetsTable", 3,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##Load", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("##Delete", ImGuiTableColumnFlags_WidthFixed, 80.0f);

			for (const std::string& name : presets) {
				ImGui::PushID(name.c_str());
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(name.c_str());

				ImGui::TableSetColumnIndex(1);
				if (ImGui::Button("Load", ImVec2(-FLT_MIN, 0))) {
					ImGuiContextLayer::LoadLayoutPreset(name);
				}

				ImGui::TableSetColumnIndex(2);
				if (ImGui::Button("Delete", ImVec2(-FLT_MIN, 0))) {
					m_PendingDeleteLayoutName = name;
					m_OpenDeleteLayoutRequest = true;
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	void EditorPreferencesPanel::RenderBehaviorTab() {
		const bool showApplication = PreferenceSectionVisible("Application", "behavior run in background window focus pause editor");
		const bool showAssetBrowser = PreferenceSectionVisible("Asset Browser", "behavior asset browser file extensions extension rename");
		const bool showAutoSave = PreferenceSectionVisible("Auto-Save", "behavior auto save autosave scene prefab interval");
		const bool showEditing = PreferenceSectionVisible("Editing", "behavior editing delete deletion confirm confirmation prompt ask dialog warn entity asset safety");
		const bool showViewport = PreferenceSectionVisible("Viewport", "behavior viewport ui alignment guides guide smart snap snapping align rect transform");
		if (!showApplication && !showAssetBrowser && !showAutoSave && !showEditing && !showViewport) return;

		if (showApplication) {
			ImGui::TextUnformatted("Application");
			ImGui::Separator();

			bool runInBackground = EditorPreferences::GetRunInBackground();
			if (ImGui::Checkbox("Run in background", &runInBackground)) {
				EditorPreferences::SetRunInBackground(runInBackground);
			}

			ImGui::Spacing();
		}

		// ── Editing ───────────────────────────────────────────────
		if (showEditing) {
			ImGui::TextUnformatted("Editing");
			ImGui::Separator();

			bool confirmOnDeleteEntity = EditorPreferences::GetConfirmOnDeleteEntity();
			if (ImGui::Checkbox("Confirm before deleting entity", &confirmOnDeleteEntity)) {
				EditorPreferences::SetConfirmOnDeleteEntity(confirmOnDeleteEntity);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Ask for confirmation before deleting an entity in the\n"
					"Entities hierarchy. Turn off to delete immediately\n"
					"without a prompt.");
			}

			bool confirmOnDeleteAsset = EditorPreferences::GetConfirmOnDeleteAsset();
			if (ImGui::Checkbox("Confirm before deleting asset", &confirmOnDeleteAsset)) {
				EditorPreferences::SetConfirmOnDeleteAsset(confirmOnDeleteAsset);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Ask for confirmation before deleting an asset in the\n"
					"Project panel. Turn off to delete immediately\n"
					"without a prompt.");
			}

			ImGui::Spacing();
		}

		// ── Viewport ──────────────────────────────────────────────
		if (showViewport) {
			ImGui::TextUnformatted("Viewport");
			ImGui::Separator();

			bool alignGuides = EditorPreferences::GetAlignmentGuidesEnabled();
			if (ImGui::Checkbox("UI alignment guides", &alignGuides)) {
				EditorPreferences::SetAlignmentGuidesEnabled(alignGuides);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"When dragging a UI (RectTransform) element in the viewport,\n"
					"show alignment guide lines against other UI elements and\n"
					"snap to them. Hold Ctrl while dragging to place freely.");
			}

			if (alignGuides) {
				float threshold = EditorPreferences::GetAlignmentSnapThreshold();
				ImGui::SetNextItemWidth(160.0f);
				if (ImGui::InputFloat("Snap distance (pixels)", &threshold, 1.0f, 4.0f, "%.0f")) {
					EditorPreferences::SetAlignmentSnapThreshold(threshold);
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"How close (in screen pixels) a dragged edge must be to\n"
						"another element's edge before it snaps.");
				}
			}

			ImGui::Spacing();
		}

		// ── Asset Browser ─────────────────────────────────────────
		if (showAssetBrowser) {
		ImGui::TextUnformatted("Asset Browser");
		ImGui::Separator();

		bool showExt = EditorPreferences::GetShowFileExtensions();
		if (ImGui::Checkbox("Show file extensions", &showExt)) {
			EditorPreferences::SetShowFileExtensions(showExt);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"When off, asset names render\n"
				"without their extension.");
		}

		ImGui::Spacing();

		// ── Auto-Save ──────────────────────────────────────────────
		}
		if (showAutoSave) {
		ImGui::TextUnformatted("Auto-Save");
		ImGui::Separator();

		bool autoSave = EditorPreferences::GetAutoSaveScenes();
		if (ImGui::Checkbox("Auto-save scenes", &autoSave)) {
			EditorPreferences::SetAutoSaveScenes(autoSave);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Periodically save the active scene while editing.\n"
				"Skipped during Play mode (Play-mode edits are discarded\n"
				"on Stop, so saving them would clobber the pre-Play snapshot).");
		}

		if (autoSave) {
			float interval = EditorPreferences::GetAutoSaveIntervalSeconds();
			ImGui::SetNextItemWidth(160.0f);
			if (ImGui::InputFloat("Interval (seconds)", &interval, 5.0f, 30.0f, "%.0f")) {
				EditorPreferences::SetAutoSaveIntervalSeconds(interval);
			}
		}

		ImGui::Spacing();

		bool autoSavePrefabs = EditorPreferences::GetAutoSavePrefabs();
		if (ImGui::Checkbox("Auto-save prefabs", &autoSavePrefabs)) {
			EditorPreferences::SetAutoSavePrefabs(autoSavePrefabs);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"In prefab edit mode, save the prefab as soon as you release\n"
				"the active inspector / hierarchy widget — no Ctrl+S needed.\n"
				"Drags and text-input debounce naturally; the save fires once\n"
				"per edit on release. Skipped during Play mode.");
		}
		}
	}

	void EditorPreferencesPanel::RenderLayoutModals() {
		// OpenPopup must run at the same id-stack scope as BeginPopupModal,
		// so emit it here (panel scope) right before the matching Begin.
		if (m_OpenSaveLayoutRequest) {
			ImGui::OpenPopup("Save Layout As");
			m_OpenSaveLayoutRequest = false;
		}
		if (m_OpenDeleteLayoutRequest) {
			ImGui::OpenPopup("Delete Layout");
			m_OpenDeleteLayoutRequest = false;
		}

		ImGuiUtils::CenterNextModal();
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::BeginPopupModal("Save Layout As", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::TextUnformatted("Save current editor layout as preset:");
			ImGui::Spacing();
			ImGui::SetNextItemWidth(320);
			if (ImGui::IsWindowAppearing()) {
				ImGui::SetKeyboardFocusHere();
			}
			const bool enterPressed = ImGui::InputText("##LayoutName",
				m_SaveLayoutBuffer, sizeof(m_SaveLayoutBuffer),
				ImGuiInputTextFlags_EnterReturnsTrue);

			const std::string nameStr(m_SaveLayoutBuffer);
			const bool nameValid = ImGuiContextLayer::IsValidLayoutPresetName(nameStr);
			if (!nameStr.empty() && !nameValid) {
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
					"Invalid name (no / \\ : * ? \" < > |, no trailing dot/space, max 64 chars).");
			} else {
				ImGui::TextDisabled("Stored under %%LOCALAPPDATA%%\\Index\\Editor\\Layouts.");
			}

			ImGui::Spacing();
			ImGui::BeginDisabled(!nameValid);
			const bool saveClicked = ImGui::Button("Save", ImVec2(100, 0));
			ImGui::EndDisabled();
			if ((saveClicked || enterPressed) && nameValid) {
				ImGuiContextLayer::SaveLayoutPreset(nameStr);
				m_SaveLayoutBuffer[0] = '\0';
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0)) ||
				ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				m_SaveLayoutBuffer[0] = '\0';
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		ImGuiUtils::CenterNextModal();
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::BeginPopupModal("Delete Layout", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::Text("Delete layout preset '%s'?", m_PendingDeleteLayoutName.c_str());
			ImGui::TextDisabled("This cannot be undone.");
			ImGui::Spacing();
			if (ImGui::Button("Delete", ImVec2(100, 0))) {
				ImGuiContextLayer::DeleteLayoutPreset(m_PendingDeleteLayoutName);
				m_PendingDeleteLayoutName.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0)) ||
				ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				m_PendingDeleteLayoutName.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

}
