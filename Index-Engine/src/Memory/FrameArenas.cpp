#include "pch.hpp"
#include "Memory/FrameArenas.hpp"

namespace Index::FrameArenas {

	namespace {

		// Storage for the two arenas. Function-local statics so we get a
		// well-defined empty-arena state before Initialize and don't depend
		// on TU-static init ordering with the tracked Allocator.
		Arena& FrameStorage() {
			static Arena s_Frame;
			return s_Frame;
		}

		Arena& PersistentStorage() {
			static Arena s_Persistent;
			return s_Persistent;
		}

		bool& InitializedFlag() {
			static bool s_Initialized = false;
			return s_Initialized;
		}

	} // anonymous namespace

	void Initialize(const FrameArenasSpec& spec) {
		FrameStorage()      = Arena(spec.FrameCapacityBytes);
		PersistentStorage() = Arena(spec.PersistentCapacityBytes);
		InitializedFlag()   = true;
	}

	void Shutdown() {
		// Move-assign empty arenas: Frame()/Persistent() remain valid references but return nullptr from Allocate.
		FrameStorage()      = Arena();
		PersistentStorage() = Arena();
		InitializedFlag()   = false;
	}

	bool IsInitialized() {
		return InitializedFlag();
	}

	Arena& Frame() {
		return FrameStorage();
	}

	Arena& Persistent() {
		return PersistentStorage();
	}

	void OnEndFrame() {
		// Reset is safe on an empty arena (no-op). We don't check
		// IsInitialized first so the call site in Application::EndFrame
		// doesn't have to know about init state.
		FrameStorage().Reset();
	}

	void ResetPersistent() {
		PersistentStorage().Reset();
	}

} // namespace Index::FrameArenas
