#pragma once

#include "Events/KeyEvent.hpp"

namespace Index {

	class KeyTypedEvent : public KeyEvent {
	public:
		explicit KeyTypedEvent(int keyCode)
			: KeyEvent(keyCode) {
		}

		IDX_EVENT_CLASS_TYPE(KeyTyped)
	};

} // namespace Index
