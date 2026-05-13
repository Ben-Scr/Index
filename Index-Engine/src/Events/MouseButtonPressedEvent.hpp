#pragma once

#include "Events/MouseButtonEvent.hpp"

namespace Index {

	class MouseButtonPressedEvent : public MouseButtonEvent {
	public:
		explicit MouseButtonPressedEvent(int button)
			: MouseButtonEvent(button) {
		}

		std::string ToString() const override {
			return std::string("MouseButtonPressedEvent: ") + std::to_string(m_Button);
		}

		IDX_EVENT_CLASS_TYPE(MouseButtonPressed)
	};

} // namespace Index
