#pragma once
#include "Core/Export.hpp"
#include "Scene/ISystem.hpp"

namespace Index {

	// Free function (not member) so UILayoutSystem and UIRenderer can both call it; INDEX_API for editor gizmo use.
	INDEX_API void ComputeUILayout(Scene& scene);

	// MUST run before UIEventSystem so hit-tests use up-to-date resolved rects.
	class UILayoutSystem : public ISystem {
	public:
		void Update(Scene& scene) override;
	};

}
