#pragma once

#include "Events/IndexEvent.hpp"

namespace Index {

	class WindowFocusEvent : public IndexEvent {
	public:
		WindowFocusEvent() = default;

		IDX_EVENT_CLASS_TYPE(WindowFocus)
		IDX_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

} // namespace Index
