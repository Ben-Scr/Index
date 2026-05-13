#include "pch.hpp"
#include "GizmosDebugSystem.hpp"

namespace Index {
	class Application;
	void GizmosDebugSystem::OnUpdate(Application& app, float dt) {
		// Intentional no-op. The editor draws its own gizmos (collider outlines,
		// transform boxes) via Index-Editor's gizmo path; running anything here
		// would double-draw. The system is kept registered so existing scenes
		// don't error on missing class lookups; the body should move to
		// Index-Editor proper before this stub is removed.
		(void)app;
		(void)dt;
	}
}
