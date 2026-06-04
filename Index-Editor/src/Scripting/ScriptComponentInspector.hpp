#pragma once
#include "Scene/Entity.hpp"

#include <span>
#include <string>

namespace Index {
	class Scene;

	void DrawScriptComponentInspector(std::span<const Entity> entities);

	// Edits are pushed to the managed instance (if alive) AND to Scene::SetSceneScriptFieldValue so they survive scene reload.
	void DrawSceneScriptFields(Scene& scene, const std::string& className);

	// Shutdown() resets picker state on Application::Reload; without it, the stale entry list from the previous project survives into the new session.
	namespace ScriptComponentInspector {
		void Shutdown();
	}
}
