#include "pch.hpp"
#include "Project/ProjectManager.hpp"
#include "Project/IndexProject.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Serialization/PrefabTemplateCache.hpp"

namespace Index {

	std::unique_ptr<IndexProject> ProjectManager::s_CurrentProject = nullptr;

	void ProjectManager::SetCurrentProject(std::unique_ptr<IndexProject> project) {
		s_CurrentProject = std::move(project);
		AssetRegistry::MarkDirty();
		AssetRegistry::Sync();

		// Invalidate stale prefab templates and re-point the file watcher at the new project root.
		PrefabTemplateCache& cache = PrefabTemplateCache::Get();
		cache.InvalidateAll();
#if INDEX_WITH_EDITOR
		if (IndexProject* live = s_CurrentProject.get(); live != nullptr && !live->AssetsDirectory.empty()) {
			cache.InitializeForProject(live->AssetsDirectory);
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
