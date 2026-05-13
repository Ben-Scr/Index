#pragma once

#include "Events/IndexEvent.hpp"

namespace Index {

	class KeyEvent : public IndexEvent {
	public:
		int GetKeyCode() const { return m_KeyCode; }

		IDX_EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryKeyboard)

	protected:
		explicit KeyEvent(int keyCode)
			: m_KeyCode(keyCode) {
		}

		int m_KeyCode;
	};

} // namespace Index
