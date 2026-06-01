#pragma once
#include "Core/Export.hpp"

#include <string>

namespace Index {

	class EditorPreferencesPanel {
	public:
		void Initialize();
		void Shutdown();
		// `pOpen` mirrors the BeginPopupModal / Begin pattern used by the
		// other panels. The close-X button on the window's title bar
		// flips it false; the caller suppresses the call next frame.
		void Render(bool* pOpen);

	private:
		void RenderAppearanceTab();
		void RenderScriptingTab();
		void RenderLayoutsTab();
		void RenderBehaviorTab();

		void RenderFontRestartModal();
		void RenderLayoutModals();
		bool IsPreferenceSearchActive() const;
		bool PreferenceSectionVisible(const char* section, const char* keywords) const;

		// Layout modal state lives on the panel (not TU-static) so OpenPopup and BeginPopupModal share the same id-stack scope.
		char m_SaveLayoutBuffer[64]{};
		bool m_OpenSaveLayoutRequest = false;
		std::string m_PendingDeleteLayoutName;
		bool m_OpenDeleteLayoutRequest = false;

		// Raised after the editor font asset changes. The typeface is
		// loaded during ImGui setup, so the user chooses when to reload.
		bool m_OpenFontRestartRequest = false;
		char m_SearchBuffer[256]{};

		// True the frame the panel becomes visible. Used to refresh
		// detection of external script editors so plugging in a new IDE
		// without restarting the editor still surfaces it.
		bool m_WasOpenLastFrame = false;
	};

}
