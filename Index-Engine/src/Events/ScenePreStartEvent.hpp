#pragma once

#include "Events/SceneEvent.hpp"

namespace Index {

	class ScenePreStartEvent : public SceneEvent {
	public:
		explicit ScenePreStartEvent(const std::string& sceneName)
			: SceneEvent(sceneName) {
		}

		IDX_EVENT_CLASS_TYPE(ScenePreStart)
	};

} // namespace Index
