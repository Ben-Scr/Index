#pragma once
#include "Core/Layer.hpp"
#include "Core/Export.hpp"

namespace Index {
	// NOTE: Despite the "System" suffix, this is intentionally a Layer (not an
	// ISystem). It is registered as an editor overlay via Application::PushOverlay
	// (see Index-Editor/EditorApplication.cpp), not via SceneDefinition::AddSystem.
	// The body is currently a no-op stub — gizmo drawing happens in the editor's
	// own gizmo path. Kept registered so existing serialized scenes don't error on
	// a missing class lookup; rename / move to Index-Editor when the stub is
	// retired. Do NOT convert to ISystem: Layers run on the Application loop and
	// have access to the editor's overlay ordering, which scene-bound ISystems do
	// not.
	class INDEX_API GizmosDebugSystem : public Layer {
	public:
		using Layer::Layer;

		void OnUpdate(Application& app, float dt) override;
	};
}
