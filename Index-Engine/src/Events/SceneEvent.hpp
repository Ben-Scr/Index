#pragma once

#include "Events/IndexEvent.hpp"

#include <string>

namespace Index {

	class SceneEvent : public IndexEvent {
	public:
		const std::string& GetSceneName() const { return m_SceneName; }

		IDX_EVENT_CLASS_CATEGORY(EventCategoryScene)

	protected:
		explicit SceneEvent(const std::string& sceneName)
			: m_SceneName(sceneName) {
		}

		std::string m_SceneName;
	};

} // namespace Index
