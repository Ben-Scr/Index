#include "pch.hpp"
#include "Audio/Backends/MiniaudioBackend.hpp"

namespace Index {

	bool MiniaudioBackend::Initialize() {
		if (m_Initialized) {
			return true;
		}

		const ma_result result = ma_engine_init(nullptr, &m_Engine);
		if (result != MA_SUCCESS) {
			IDX_CORE_ERROR("[{}] MiniaudioBackend: ma_engine_init failed. Error: {}",
				ErrorCodeToString(IndexErrorCode::LoadFailed), static_cast<int>(result));
			return false;
		}

		m_Initialized = true;
		return true;
	}

	void MiniaudioBackend::Shutdown() {
		if (!m_Initialized) {
			return;
		}

		ma_engine_uninit(&m_Engine);
		m_Initialized = false;
	}

	void MiniaudioBackend::SetMasterVolume(float volume) {
		if (!m_Initialized) {
			return;
		}

		ma_engine_set_volume(&m_Engine, volume);
	}

	void MiniaudioBackend::SetSuspended(bool suspended) {
		if (!m_Initialized) {
			return;
		}

		// Stop/start the device thread so playback freezes in place and later resumes from the same cursor.
		ma_device* device = ma_engine_get_device(&m_Engine);
		if (!device) {
			return;
		}

		if (suspended) {
			if (ma_device_is_started(device)) {
				ma_device_stop(device);
			}
		}
		else if (!ma_device_is_started(device)) {
			ma_device_start(device);
		}
	}

}
