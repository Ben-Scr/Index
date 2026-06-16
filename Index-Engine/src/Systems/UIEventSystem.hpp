#pragma once
#include "Scene/EntityHandle.hpp"
#include "Scene/ISystem.hpp"

#include <entt/entt.hpp>

namespace Index {

	class UIEventSystem : public ISystem {
	public:
		void Update(Scene& scene) override;
		// OnPreRender keeps slider visuals in sync with inspector edits in edit mode (Update doesn't run outside play).
		void OnPreRender(Scene& scene) override;
		void OnDestroy(Scene&) override { m_PressedEntity = entt::null; m_HoveredEntity = entt::null; }

		// Front-most Interactable entity under the pointer this frame, or entt::null. Already gated on an
		// enabled InteractableComponent; surfaced to C# via UIPointer.HoveredEntity.
		EntityHandle GetHoveredEntity() const { return m_HoveredEntity; }

	private:
		EntityHandle m_PressedEntity = entt::null;
		EntityHandle m_HoveredEntity = entt::null;
	};

}
