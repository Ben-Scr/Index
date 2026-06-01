#include "pch.hpp"
#include "GizmosDebugSystem.hpp"

namespace Index {
	class Application;
	void GizmosDebugSystem::OnUpdate(Application& app, float dt) {
		// No-op: editor draws gizmos via its own path; adding anything here would double-draw.
		(void)app;
		(void)dt;
	}
}
