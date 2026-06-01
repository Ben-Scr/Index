#include "pch.hpp"
#include "Memory/Arena.hpp"

#include <cstdlib>
#include <new>

namespace Index {

	namespace {

		constexpr bool IsPowerOfTwo(std::size_t value) noexcept {
			return value != 0 && (value & (value - 1)) == 0;
		}

		constexpr std::size_t AlignUp(std::size_t offset, std::size_t alignment) noexcept {
			const std::size_t mask = alignment - 1;
			return (offset + mask) & ~mask;
		}

	} // anonymous namespace

	Arena::Arena(std::size_t capacity)
		: m_Capacity(capacity)
	{
		if (capacity == 0) {
			return;
		}

		// std::malloc bypasses the tracked allocator (avoids per-pointer overhead) and works before Allocator::Init().
		void* memory = std::malloc(capacity);
		if (!memory) {
			m_Capacity = 0;
			throw std::bad_alloc();
		}
		m_Base = static_cast<std::byte*>(memory);
	}

	Arena::~Arena() {
		if (m_Base) {
			std::free(m_Base);
		}
	}

	Arena::Arena(Arena&& other) noexcept
		: m_Base(other.m_Base)
		, m_Capacity(other.m_Capacity)
		, m_Offset(other.m_Offset)
	{
		other.m_Base = nullptr;
		other.m_Capacity = 0;
		other.m_Offset = 0;
	}

	Arena& Arena::operator=(Arena&& other) noexcept {
		if (this == &other) {
			return *this;
		}

		if (m_Base) {
			std::free(m_Base);
		}

		m_Base = other.m_Base;
		m_Capacity = other.m_Capacity;
		m_Offset = other.m_Offset;

		other.m_Base = nullptr;
		other.m_Capacity = 0;
		other.m_Offset = 0;

		return *this;
	}

	void* Arena::Allocate(std::size_t size, std::size_t alignment) {
		IDX_CORE_ASSERT(IsPowerOfTwo(alignment),
			"Arena::Allocate alignment must be a power of two");

		if (size == 0 || !m_Base) {
			return nullptr;
		}

		// Align the absolute address (not the offset): malloc's 16-byte guarantee is insufficient for >16-byte alignments.
		const std::uintptr_t baseAddr    = reinterpret_cast<std::uintptr_t>(m_Base);
		const std::uintptr_t currentAddr = baseAddr + m_Offset;
		const std::uintptr_t alignedAddr = AlignUp(currentAddr, alignment);
		const std::size_t    aligned     = static_cast<std::size_t>(alignedAddr - baseAddr);

		// Overflow-safe capacity check: compare against Capacity - aligned
		// rather than aligned + size so we can't roll over size_t on a huge
		// request.
		if (aligned > m_Capacity || size > m_Capacity - aligned) {
			return nullptr;
		}

		void* memory = m_Base + aligned;
		m_Offset = aligned + size;
		return memory;
	}

	void Arena::Reset(std::size_t mark) noexcept {
		// Clamp rather than assert: a stale Mark from before a move-assignment may exceed the new capacity.
		m_Offset = mark < m_Capacity ? mark : m_Capacity;
	}

} // namespace Index
