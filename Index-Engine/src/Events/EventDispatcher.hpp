#pragma once

#include "Events/IndexEvent.hpp"

namespace Index {

	class EventDispatcher {
	public:
		explicit EventDispatcher(IndexEvent& event)
			: m_Event(event) {
		}

		template<typename T, typename F>
		bool Dispatch(F&& func) {
			if (m_Event.GetEventType() == T::GetStaticType() && !m_Event.Handled) {
				m_Event.Handled = func(static_cast<T&>(m_Event));
				return true;
			}
			return false;
		}

	private:
		IndexEvent& m_Event;
	};

} // namespace Index
