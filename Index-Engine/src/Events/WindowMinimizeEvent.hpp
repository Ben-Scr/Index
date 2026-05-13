#pragma once

#include "Events/IndexEvent.hpp"

namespace Index {

	class WindowMinimizeEvent : public IndexEvent {
	public:
		explicit WindowMinimizeEvent(bool minimized)
			: m_Minimized(minimized) {
		}

		bool IsMinimized() const { return m_Minimized; }

		IDX_EVENT_CLASS_TYPE(WindowMinimize)
		IDX_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		bool m_Minimized;
	};

} // namespace Index
