#pragma once

#include "Events/IndexEvent.hpp"

namespace Index {

	class WindowLostFocusEvent : public IndexEvent {
	public:
		WindowLostFocusEvent() = default;

		IDX_EVENT_CLASS_TYPE(WindowLostFocus)
		IDX_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

} // namespace Index
