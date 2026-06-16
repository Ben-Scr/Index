#include "pch.hpp"
#include "Project/ProjectManager.hpp"
#include "Project/IndexProject.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Serialization/PrefabTemplateCache.hpp"
#include "Core/Application.hpp"

namespace Index {

	std::unique_ptr<IndexProject> ProjectManager::s_CurrentProject = nullptr;

	void ProjectManager::SetCurrentProject(std::unique_ptr<IndexProject> project, bool assetsImmutable) {
		s_CurrentProject = std::move(project);

		// Set immutability before anything triggers a rebuild (see header). Standalone runtimes then
		// trust the shipped manifest; the editor stays mutable so on-disk edits are detected.
		AssetRegistry::SetAssetsImmutable(assetsImmutable);

		// Mark dirty but DON'T Sync() here: the rebuild is the multi-second walk on an asset-heavy
		// project, and SetCurrentProject runs before the window exists (CreateApplication). Leaving it
		// dirty lets the first lazy consumer — the deferred startup scene load, which runs behind the
		// splash — trigger the single rebuild, off the pre-window critical path.
		AssetRegistry::MarkDirty();

		// Invalidate stale prefab templates and re-point the file watcher at the new project root.
		PrefabTemplateCache& cache = PrefabTemplateCache::Get();
		cache.InvalidateAll();
#if INDEX_WITH_EDITOR
		// The .prefab auto-invalidation watcher is an editor-authoring convenience: a
		// standalone runtime never edits prefabs, so starting it there only pays a recursive
		// Assets/ walk for nothing. Gate on the editor host at runtime — INDEX_WITH_EDITOR is
		// still 1 in Development standalone builds (only Dist strips it).
		if (Application::IsEditor()
			&& s_CurrentProject != nullptr && !s_CurrentProject->AssetsDirectory.empty()) {
			cache.InitializeForProject(s_CurrentProject->AssetsDirectory);
		}
		else {
			cache.Shutdown();
		}
#endif
	}

	IndexProject* ProjectManager::GetCurrentProject() {
		return s_CurrentProject.get();
	}

	bool ProjectManager::HasProject() {
		return s_CurrentProject != nullptr;
	}

} // namespace Index
