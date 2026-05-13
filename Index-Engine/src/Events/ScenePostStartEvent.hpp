#pragma once

#include "Events/SceneEvent.hpp"

namespace Index {

	class ScenePostStartEvent : public SceneEvent {
	public:
		explicit ScenePostStartEvent(const std::string& sceneName)
			: SceneEvent(sceneName) {
		}

		IDX_EVENT_CLASS_TYPE(ScenePostStart)
	};

} // namespace Index
