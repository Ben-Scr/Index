#pragma once

#include "Events/IndexEvent.hpp"

namespace Index {

	class MouseScrolledEvent : public IndexEvent {
	public:
		MouseScrolledEvent(float xOffset, float yOffset)
			: m_XOffset(xOffset), m_YOffset(yOffset) {
		}

		float GetXOffset() const { return m_XOffset; }
		float GetYOffset() const { return m_YOffset; }

		std::string ToString() const override {
			return std::string("MouseScrolledEvent: ") + std::to_string(m_XOffset) + ", " + std::to_string(m_YOffset);
		}

		IDX_EVENT_CLASS_TYPE(MouseScrolled)
		IDX_EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryMouse)

	private:
		float m_XOffset;
		float m_YOffset;
	};

} // namespace Index
