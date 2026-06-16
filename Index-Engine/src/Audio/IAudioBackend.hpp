#pragma once

#include "Core/Export.hpp"

// Forward-declare miniaudio's engine type so this header is cheap to include.
// The full miniaudio.h is heavy (single-file library) and we don't want every
// backend consumer to pay for it.
struct ma_engine;

namespace Index {

	class INDEX_API IAudioBackend {
	public:
		virtual ~IAudioBackend() = default;

		// Lifecycle. Initialize creates the platform audio device; Shutdown tears
		// it down. AudioManager::Initialize / Shutdown invoke these.
		virtual bool Initialize() = 0;
		virtual void Shutdown() = 0;

		// Master volume in [0, 1]. AudioManager clamps before dispatch.
		virtual void SetMasterVolume(float volume) = 0;

		// Suspend/resume the whole audio device when the app is background-paused.
		// Suspended freezes every sound in place; resuming continues from the same cursor (a pause, not a stop).
		// Default no-op for backends without a stoppable device.
		virtual void SetSuspended(bool suspended) { (void)suspended; }

		// Escape hatch: nullptr-safe; will be removed once all AudioManager ma_* calls are folded into virtual methods.
		virtual ma_engine* GetMiniaudioEngine() { return nullptr; }
	};

}
