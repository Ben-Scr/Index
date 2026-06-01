#pragma once

#include "Scene/Entity.hpp"

#include <span>

namespace Index {
	class SceneManager;
	struct ComponentInfo;

	void RegisterEditorComponentInspectors(SceneManager& sceneManager);

	// Use this instead of info.drawInspector directly; falls back to the PropertyDrawer auto-drawer when no custom lambda is set.
	void DispatchComponentInspector(const ComponentInfo& info, std::span<const Entity> entities);
}
