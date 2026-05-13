#pragma once

#include "Events/IndexEvent.hpp"

namespace Index {

	class WindowCloseEvent : public IndexEvent {
	public:
		WindowCloseEvent() = default;

		IDX_EVENT_CLASS_TYPE(WindowClose)
		IDX_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

} // namespace Index
