#pragma once
#include "Project/IndexProject.hpp"
#include "Core/Export.hpp"
#include <memory>

namespace Index {

	class INDEX_API ProjectManager {
	public:
		// assetsImmutable=true (standalone runtime) lets the registry trust the shipped manifest and
		// skip the directory walk. Must be set here, inside engine.dll, because AssetRegistry is a
		// header-only class with per-binary static state — a runtime-exe call wouldn't reach the copy
		// that runs the rebuild.
		static void SetCurrentProject(std::unique_ptr<IndexProject> project, bool assetsImmutable = false);
		static IndexProject* GetCurrentProject();
		static bool HasProject();

	private:
		static std::unique_ptr<IndexProject> s_CurrentProject;
	};

} // namespace Index
