#pragma once

#include "Events/SceneEvent.hpp"

namespace Index {

	class ScenePreStopEvent : public SceneEvent {
	public:
		explicit ScenePreStopEvent(const std::string& sceneName)
			: SceneEvent(sceneName) {
		}

		IDX_EVENT_CLASS_TYPE(ScenePreStop)
	};

} // namespace Index
