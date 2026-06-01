#pragma once

#include "Core/Application.hpp"

namespace Index {
	class ApplicationEditorAccess {
	public:
		static void SetPlaymodePaused(bool paused)
		{
			if (Application::s_Instance) {
				Application::s_Instance->m_IsGameplayPaused = paused;
			}
		}

		static bool IsPlaymodePaused()
		{
			return Application::s_Instance ? Application::s_Instance->m_IsGameplayPaused : false;
		}

		static void SetGameInputEnabled(bool enabled)
		{
			if (Application::s_Instance) {
				Application::s_Instance->m_IsScriptInputEnabled = enabled;
			}
		}

		static bool IsGameInputEnabled()
		{
			return Application::s_Instance ? Application::s_Instance->m_IsScriptInputEnabled : true;
		}

		// Application.Quit() in the editor raises this flag instead of closing the process; poll+consume each frame via the normal stop-play path.
		static bool ConsumeQuitStopPlayRequest()
		{
			if (!Application::s_Instance) return false;
			const bool requested = Application::s_Instance->m_EditorStopPlayRequested;
			Application::s_Instance->m_EditorStopPlayRequested = false;
			return requested;
		}

		// Note: `RequestEditorStopPlay()` lives on `Application` (Core) because the
		// engine-side script bindings (ScriptBindingsScene) need to signal the editor
		// without depending on editor-side headers. Don't duplicate it here.

		// Resets Time.TimeSinceStartup / Time.RealtimeSinceStartup to zero. Called
		// at editor play-mode entry so every play session starts at t=0; built games
		// already get this for free at the end of Application::Initialize.
		static void MarkGameStart()
		{
			if (Application::s_Instance) {
				Application::s_Instance->m_Time.MarkGameStart();
			}
		}

	private:
		ApplicationEditorAccess() = delete;
	};
}
