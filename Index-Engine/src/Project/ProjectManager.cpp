#include "pch.hpp"
#include "Project/ProjectManager.hpp"
#include "Assets/AssetRegistry.hpp"

namespace Index {

	std::unique_ptr<IndexProject> ProjectManager::s_CurrentProject = nullptr;

	void ProjectManager::SetCurrentProject(std::unique_ptr<IndexProject> project) {
		s_CurrentProject = std::move(project);
		AssetRegistry::MarkDirty();
		AssetRegistry::Sync();
	}

	IndexProject* ProjectManager::GetCurrentProject() {
		return s_CurrentProject.get();
	}

	bool ProjectManager::HasProject() {
		return s_CurrentProject != nullptr;
	}

} // namespace Index
