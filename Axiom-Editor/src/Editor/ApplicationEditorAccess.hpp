#pragma once

#include "Core/Application.hpp"

namespace Axiom {
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
