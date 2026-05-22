#pragma once

#include "Events/IndexEvent.hpp"

namespace Index {

	class WindowMaximizeEvent : public IndexEvent {
	public:
		explicit WindowMaximizeEvent(bool maximized)
			: m_Maximized(maximized) {
		}

		bool IsMaximized() const { return m_Maximized; }

		IDX_EVENT_CLASS_TYPE(WindowMaximize)
		IDX_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		bool m_Maximized;
	};

} // namespace Index
