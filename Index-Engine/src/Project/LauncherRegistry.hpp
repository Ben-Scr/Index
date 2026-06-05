#pragma once
#include "Core/Export.hpp"
#include "Project/LauncherProjectEntry.hpp"

#include <string>
#include <vector>

namespace Index {

	class INDEX_API LauncherRegistry {
	public:
		void Load();
		void Save();

		const std::vector<LauncherProjectEntry>& GetProjects() const { return m_Projects; }
		void AddProject(const std::string& name, const std::string& path);
		void RemoveProject(const std::string& path);
		void UpdateLastOpened(const std::string& path);
		// Updates the launcher's display label only — does NOT modify the project's on-disk project.json.
		bool RenameProject(const std::string& path, const std::string& newName);
		void ValidateAll();

	private:
		static std::string GetRegistryPath();
		std::vector<LauncherProjectEntry> m_Projects;
	};

} // namespace Index
