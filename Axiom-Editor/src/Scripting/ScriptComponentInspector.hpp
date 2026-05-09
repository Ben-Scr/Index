#pragma once
#include "Scene/Entity.hpp"

#include <span>
#include <string>

namespace Axiom {
	class Scene;

	void DrawScriptComponentInspector(std::span<const Entity> entities);

	// Render a GameSystem's [ShowInEditor] fields into the inspector. Reads
	// from the live managed instance (when the scene is in play / Awake'd)
	// or from class defaults patched with the scene's stored field
	// overrides. Edits are pushed both to the managed instance (if alive)
	// and to Scene::SetGameSystemFieldValue so they survive scene reload.
	void DrawGameSystemFields(Scene& scene, const std::string& className);

	// M28: TU-statics for the "Add Script" picker (open flag, search buffer,
	// cached entries, target entity) used to live as four loose anonymous-
	// namespace globals. Wrapped into PickerState so a single Shutdown()
	// can reset them on Application::Reload — without this, the picker's
	// stale entry list (referencing the previous project's filesystem)
	// could survive into the new session.
	namespace ScriptComponentInspector {
		void Shutdown();
	}
}
