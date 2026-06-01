#pragma once
#include "Scene/EntityHandle.hpp"
#include "Scene/ISystem.hpp"

#include <entt/entt.hpp>

namespace Index {

	// MUST run before UIEventSystem: writes ActivatedThisFrame and IsFocused so UIEventSystem's hit-test sees them; when an InputField owns focus it surrenders arrow keys to UIEventSystem's caret handler.
	class UIFocusSystem : public ISystem {
	public:
		void Update(Scene& scene) override;
		void OnDestroy(Scene&) override {
			m_FocusedEntity = entt::null;
			m_PrevAxisLeftPushed = false;
			m_PrevAxisRightPushed = false;
			m_PrevAxisUpPushed = false;
			m_PrevAxisDownPushed = false;
		}

	private:
		// Persistent across frames so Tab navigation is sticky between
		// updates. Cleared on Cancel and when the entity is destroyed,
		// disabled, or has its Focusable flag turned off at runtime.
		EntityHandle m_FocusedEntity = entt::null;

		// Used by left-stick / D-pad bindings to fire one nav step per
		// "deflection" (rising edge) instead of streaming a step every
		// frame the stick is pushed.
		bool m_PrevAxisLeftPushed  = false;
		bool m_PrevAxisRightPushed = false;
		bool m_PrevAxisUpPushed    = false;
		bool m_PrevAxisDownPushed  = false;
	};

}
