#pragma once

#include "Events/KeyEvent.hpp"

namespace Index {

	class KeyReleasedEvent : public KeyEvent {
	public:
		explicit KeyReleasedEvent(int keyCode)
			: KeyEvent(keyCode) {
		}

		std::string ToString() const override {
			return std::string("KeyReleasedEvent: ") + std::to_string(m_KeyCode);
		}

		IDX_EVENT_CLASS_TYPE(KeyReleased)
	};

} // namespace Index
