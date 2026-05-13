#pragma once

#include "Events/IndexEvent.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Index {

	class FileDropEvent : public IndexEvent {
	public:
		explicit FileDropEvent(std::vector<std::string> paths)
			: m_Paths(std::move(paths)) {
		}

		const std::vector<std::string>& GetPaths() const { return m_Paths; }
		int GetCount() const { return static_cast<int>(m_Paths.size()); }

		IDX_EVENT_CLASS_TYPE(FileDrop)
		IDX_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		std::vector<std::string> m_Paths;
	};

} // namespace Index
