#pragma once

#include "Memory/Arena.hpp"

#include <cstddef>
#include <new>
#include <type_traits>

namespace Index {

	// STL allocator adapter that bump-allocates from an Arena. deallocate() is a no-op; pre-reserve to avoid orphaned blocks. T must be trivially destructible.
	template <class T>
	class ArenaAllocator {
	public:
		using value_type = T;

		using propagate_on_container_copy_assignment = std::true_type;
		using propagate_on_container_move_assignment = std::true_type;
		using propagate_on_container_swap            = std::true_type;

		// Two allocators are equal iff they point at the same arena.
		// is_always_equal must be false because we *can* be unequal.
		using is_always_equal = std::false_type;

		ArenaAllocator() noexcept = default;

		explicit ArenaAllocator(Arena& arena) noexcept
			: m_Arena(&arena)
		{
		}

		// Rebind ctor: required so containers can ask the allocator for an
		// allocator-of-a-different-type (e.g. vector's internal node type).
		template <class U>
		ArenaAllocator(const ArenaAllocator<U>& other) noexcept
			: m_Arena(other.GetArena())
		{
		}

		T* allocate(std::size_t n) {
			if (n == 0) {
				return nullptr;
			}
			if (!m_Arena) {
				throw std::bad_alloc();
			}
			void* memory = m_Arena->Allocate(sizeof(T) * n, alignof(T));
			if (!memory) {
				throw std::bad_alloc();
			}
			return static_cast<T*>(memory);
		}

		// No-op: the arena reclaims storage only via Reset(). Containers
		// still call deallocate on destruction; that's fine, it just
		// doesn't shrink the arena.
		void deallocate(T* /*p*/, std::size_t /*n*/) noexcept {
		}

		Arena* GetArena() const noexcept { return m_Arena; }

	private:
		Arena* m_Arena = nullptr;
	};

	template <class T, class U>
	bool operator==(const ArenaAllocator<T>& a, const ArenaAllocator<U>& b) noexcept {
		return a.GetArena() == b.GetArena();
	}

	template <class T, class U>
	bool operator!=(const ArenaAllocator<T>& a, const ArenaAllocator<U>& b) noexcept {
		return !(a == b);
	}

} // namespace Index
