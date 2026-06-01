#pragma once
#include "Core/Export.hpp"
#include "Scene/ISystem.hpp"

namespace Index {
	// Physics body transforms are left alone (engine-written), but their children are still recursed into.
	class TransformHierarchySystem : public ISystem {
	public:
		void Awake(Scene& scene) override;
		void Update(Scene& scene) override;
		void OnPreRender(Scene& scene) override;

		// INDEX_API: editor calls Propagate() directly between inspector input and viewport draw so edits appear same-frame.
		static INDEX_API void Propagate(Scene& scene);
	};
}
