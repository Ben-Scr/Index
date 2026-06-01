#pragma once

#include "Memory/Arena.hpp"

#include <cstddef>

namespace Index {

	// RAII mark/restore for an Arena; non-copyable/non-movable — must be a strict stack scope.
	class ScopedArena {
	public:
		explicit ScopedArena(Arena& arena) noexcept
			: m_Arena(arena)
			, m_Mark(arena.Mark())
		{
		}

		~ScopedArena() {
			m_Arena.Reset(m_Mark);
		}

		ScopedArena(const ScopedArena&) = delete;
		ScopedArena& operator=(const ScopedArena&) = delete;
		ScopedArena(ScopedArena&&) = delete;
		ScopedArena& operator=(ScopedArena&&) = delete;

		// The mark this scope will restore on destruction. Useful for
		// asserting in tests / debug builds that nested arena use behaves.
		std::size_t Mark() const noexcept { return m_Mark; }

	private:
		Arena&      m_Arena;
		std::size_t m_Mark;
	};

} // namespace Index
