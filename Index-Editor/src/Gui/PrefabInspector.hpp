#pragma once
#include "Scene/EntityHandle.hpp"
#include <memory>
#include <string>

namespace Index {

	class Scene;
	namespace Json { class Value; }

	// Inspector for a .prefab asset backed by a detached Scene. Component-destroy hooks that touch global subsystems are gated by Scene::IsDetached().
	class PrefabInspector {
	public:
		PrefabInspector();
		~PrefabInspector();

		PrefabInspector(const PrefabInspector&) = delete;
		PrefabInspector& operator=(const PrefabInspector&) = delete;

		// Load `prefabPath` into a fresh detached scene. Replaces any prefab
		// currently open without prompting — the caller is responsible for
		// driving the save/discard prompt via `HasUnsavedChanges()` first.
		void Open(const std::string& prefabPath);

		// Tear down the detached scene. Caller should check `HasUnsavedChanges`.
		void Close();

		bool IsOpen() const { return m_PrefabScene != nullptr; }
		const std::string& GetCurrentPath() const { return m_PrefabPath; }
		bool HasUnsavedChanges() const;

		// Render the inspector body for the prefab's root entity. Returns true
		// if the user edited anything this frame (caller can use as a UX hint).
		bool Render();

		bool Save();

	private:
		void PropagateToLiveInstances(uint64_t prefabGuid, const Json::Value& previousSourceEntity);

		std::unique_ptr<Scene> m_PrefabScene;
		EntityHandle m_RootEntity = entt::null;
		std::string m_PrefabPath;
		char m_AddComponentSearchBuffer[128]{};
	};

}
