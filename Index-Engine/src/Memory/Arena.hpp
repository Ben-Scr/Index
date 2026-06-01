#pragma once

#include "Core/Export.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace Index {

	// Linear (bump) allocator. O(1) alloc, O(1) Reset(). Non-growing: Allocate() returns nullptr on exhaustion. Not thread-safe.
	class INDEX_CORE_API Arena {
	public:
		Arena() noexcept = default;

		// Allocate a fixed backing buffer of `capacity` bytes. `capacity == 0`
		// is valid and produces an empty arena (every Allocate returns nullptr).
		explicit Arena(std::size_t capacity);

		~Arena();

		Arena(const Arena&) = delete;
		Arena& operator=(const Arena&) = delete;

		Arena(Arena&& other) noexcept;
		Arena& operator=(Arena&& other) noexcept;

		// Raw aligned allocation. Returns nullptr when the arena cannot fit
		// the request (or when `size == 0`). Alignment must be a power of two.
		void* Allocate(std::size_t size,
			std::size_t alignment = alignof(std::max_align_t));

		template <class T, class... Args>
		T* Create(Args&&... args) {
			static_assert(std::is_trivially_destructible_v<T>,
				"Arena::Create<T> requires a trivially-destructible T. "
				"Non-trivial destructors will not run on Reset.");

			void* memory = Allocate(sizeof(T), alignof(T));
			if (!memory) {
				return nullptr;
			}
			return new (memory) T(std::forward<Args>(args)...);
		}

		template <class T>
		std::span<T> CreateArray(std::size_t count) {
			static_assert(std::is_trivially_destructible_v<T>,
				"Arena::CreateArray<T> requires a trivially-destructible T.");

			if (count == 0) {
				return {};
			}
			void* memory = Allocate(sizeof(T) * count, alignof(T));
			if (!memory) {
				return {};
			}
			return std::span<T>(static_cast<T*>(memory), count);
		}

		// Returns the current bump offset. Pair with Reset(mark) to rewind
		// to this point — the classic scratch-and-restore pattern. See also
		// ScopedArena for an RAII wrapper.
		std::size_t Mark() const noexcept { return m_Offset; }

		// Rewind to `mark` (0 = full rewind). Bytes past `mark` become
		// available for reuse. No destructors run; that's the trade.
		void Reset(std::size_t mark = 0) noexcept;

		std::size_t Capacity()  const noexcept { return m_Capacity; }
		std::size_t Used()      const noexcept { return m_Offset; }
		std::size_t Remaining() const noexcept { return m_Capacity - m_Offset; }

		// Backing buffer base. Mostly for diagnostics / debugger inspection;
		// don't poke at the bytes directly in normal code.
		const std::byte* Data() const noexcept { return m_Base; }

	private:
		std::byte*  m_Base     = nullptr;
		std::size_t m_Capacity = 0;
		std::size_t m_Offset   = 0;
	};

} // namespace Index
