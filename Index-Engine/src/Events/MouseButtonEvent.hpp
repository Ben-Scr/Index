#pragma once

#include "Events/IndexEvent.hpp"

namespace Index {

	class MouseButtonEvent : public IndexEvent {
	public:
		int GetButton() const { return m_Button; }

		IDX_EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryMouse | EventCategoryMouseButton)

	protected:
		explicit MouseButtonEvent(int button)
			: m_Button(button) {
		}

		int m_Button;
	};

} // namespace Index
