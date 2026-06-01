#pragma once

#include "Core/Export.hpp"
#include "Memory/Arena.hpp"

#include <cstddef>

namespace Index {

	struct FrameArenasSpec {
		std::size_t FrameCapacityBytes      = 1024 * 1024;   // 1 MiB
		std::size_t PersistentCapacityBytes = 64   * 1024;   // 64 KiB
	};

	// Two main-thread scratch arenas: Frame() (auto-reset each frame) and Persistent() (manual reset).
	// NOT thread-safe — worker threads should hold a local Arena instead.
	namespace FrameArenas {

		// Allocates backing buffers per the spec. Calling Initialize again
		// frees the existing buffers and allocates new ones.
		INDEX_CORE_API void Initialize(const FrameArenasSpec& spec = FrameArenasSpec{});

		// Frees backing buffers. Safe to call when never Initialize'd.
		INDEX_CORE_API void Shutdown();

		// True between Initialize and Shutdown. Mostly for assertions.
		INDEX_CORE_API bool IsInitialized();

		INDEX_CORE_API Arena& Frame();

		// Persistent scratch arena. Never auto-reset.
		INDEX_CORE_API Arena& Persistent();

		// Hook called from Application::EndFrame. Wipes Frame() in O(1).
		// Persistent() is untouched. Safe to call when never Initialize'd.
		INDEX_CORE_API void OnEndFrame();

		INDEX_CORE_API void ResetPersistent();

	} // namespace FrameArenas

} // namespace Index
