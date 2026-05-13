#pragma once

#include "Events/IndexEvent.hpp"

namespace Index {

	class WindowResizeEvent : public IndexEvent {
	public:
		WindowResizeEvent(int width, int height)
			: m_Width(width), m_Height(height) {
		}

		int GetWidth() const { return m_Width; }
		int GetHeight() const { return m_Height; }

		std::string ToString() const override {
			return std::string("WindowResizeEvent: ") + std::to_string(m_Width) + ", " + std::to_string(m_Height);
		}

		IDX_EVENT_CLASS_TYPE(WindowResize)
		IDX_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		int m_Width;
		int m_Height;
	};

} // namespace Index
