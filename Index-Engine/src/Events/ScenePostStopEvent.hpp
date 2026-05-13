#pragma once

#include "Events/SceneEvent.hpp"

namespace Index {

	class ScenePostStopEvent : public SceneEvent {
	public:
		explicit ScenePostStopEvent(const std::string& sceneName)
			: SceneEvent(sceneName) {
		}

		IDX_EVENT_CLASS_TYPE(ScenePostStop)
	};

} // namespace Index
