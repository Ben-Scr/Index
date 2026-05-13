#pragma once
#include "Project/IndexProject.hpp"
#include "Core/Export.hpp"
#include <memory>

namespace Index {

	class INDEX_API ProjectManager {
	public:
		static void SetCurrentProject(std::unique_ptr<IndexProject> project);
		static IndexProject* GetCurrentProject();
		static bool HasProject();

	private:
		static std::unique_ptr<IndexProject> s_CurrentProject;
	};

} // namespace Index
