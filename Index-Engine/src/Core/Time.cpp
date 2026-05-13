#include "pch.hpp"
#include "Time.hpp"
#include <Math/Common.hpp>

namespace Index {
	void Time::SetTargetFramerate(float framerate) {
		if (framerate <= 0.0f) {
			m_TargetFPS = 0.0f;
			m_UpdateDeltaTime = std::numeric_limits<float>::infinity();
			m_FrameDuration = std::chrono::steady_clock::duration::max();
			return;
		}

		m_TargetFPS = framerate;
		m_UpdateDeltaTime = 1.0f / m_TargetFPS;
		m_FrameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
			std::chrono::duration<float>(m_UpdateDeltaTime)
		);
	}

	void Time::SetTimeScale(float scale) {
		m_TimeScale = std::max(0.0f, scale);
	}

	void Time::SetFixedDeltaTime(float step) {
		// 0 silently disables FixedUpdate (Application::Run zeroes the accumulator
		// and skips dispatch when fixedDt <= 0). Clamp to a sane minimum and warn
		// so misconfigured callers don't get mysteriously dead physics + scripts.
		constexpr float k_MinFixedStep = 1.f / 240.f;
		if (step <= 0.f) {
			IDX_CORE_WARN_TAG("Time", "SetFixedDeltaTime({}) is non-positive; clamping to {} ({} Hz). FixedUpdate would otherwise be disabled.", step, k_MinFixedStep, 1.f / k_MinFixedStep);
			step = k_MinFixedStep;
		}
		step = Clamp(step, k_MinFixedStep, 1.f);
		m_FixedDeltaTime = step;
	}

	float Time::GetElapsedTime() const {
		std::chrono::duration<float> elapsed = Clock::now() - m_StartTime;
		return elapsed.count();
	}

	float Time::GetSimulatedElapsedTime() const { return m_SimulatedElapsedTime; }

	float Time::GetRealtimeSinceStartup() const {
		if (!m_GameStarted) return 0.0f;
		std::chrono::duration<float> elapsed = Clock::now() - m_GameStartTime;
		return elapsed.count();
	}

	void Time::MarkGameStart() {
		m_GameStartTime = Clock::now();
		m_GameSimulatedElapsedTime = 0.0f;
		m_GameStarted = true;
	}

	void Time::Update(float deltaTime) {
		m_DeltaTime = deltaTime;
		m_SimulatedElapsedTime += m_DeltaTime * m_TimeScale;
		if (m_GameStarted) {
			m_GameSimulatedElapsedTime += m_DeltaTime * m_TimeScale;
		}
	}
}
