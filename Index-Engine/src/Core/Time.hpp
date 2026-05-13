#pragma once
#include <chrono>
#include "Core/Export.hpp"

namespace Index {
	class Application;
	class ApplicationEditorAccess;

	class INDEX_API Time {
	public:
		float GetDeltaTime() const { return m_DeltaTime * m_TimeScale; }
		float GetFrameRate() const { return m_DeltaTime > 0.0f ? 1.0f / m_DeltaTime : 0.0f; }
		float GetDeltaTimeUnscaled() const { return m_DeltaTime; }
		float GetUnscaledDeltaTime() const { return m_DeltaTime; }
		void SetTargetFramerate(float fps);

		// Always returns the unscaled fixed step. TimeScale changes step *frequency*
		// (more/fewer FixedUpdate calls per real second), not the per-call dt — so
		// integration done in FixedUpdate scales correctly without compounding.
		float GetFixedDeltaTime() const { return m_FixedDeltaTime; }
		void SetFixedDeltaTime(float step);

		float GetUnscaledFixedDeltaTime() const { return m_FixedDeltaTime; }

		float GetTimeScale() const { return m_TimeScale; }
		void SetTimeScale(float scale);

		// Note: Realtime elapsed time
		float GetElapsedTime() const;
		// Note: Elapsed time based on timescale
		float GetSimulatedElapsedTime() const;

		// Time since the game started (engine init excluded). Scales with TimeScale —
		// at 2x scale the value advances twice as fast as wallclock. Reset by
		// MarkGameStart(); zero before the first call.
		float GetTimeSinceStartup() const { return m_GameSimulatedElapsedTime; }

		// Time since the game started (engine init excluded). Wallclock — ignores
		// TimeScale and pause. Reset by MarkGameStart(); zero before the first call.
		float GetRealtimeSinceStartup() const;

		int GetFrameCount() const { return m_FrameCount; }

	private:
		using Clock = std::chrono::steady_clock;

		void Update(float deltaTime);
		void AdvanceFrameCount() { m_FrameCount++; }

		// Resets the "game start" baseline used by GetTimeSinceStartup /
		// GetRealtimeSinceStartup. Called once at the end of Application::Initialize
		// (after RaiseApplicationStart) so the engine-init pipeline is excluded, and
		// again on every editor play-mode entry so each play session starts at zero.
		void MarkGameStart();

		float m_DeltaTime = 0.0f;
		float m_TargetFPS = 144.f;
		float m_TimeScale = 1.f;
		float m_UpdateDeltaTime = 1.0f / m_TargetFPS;
		float m_FixedDeltaTime = 1.0f / 50.f;
		float m_SimulatedElapsedTime = 0.0f;
		float m_GameSimulatedElapsedTime = 0.0f;
		bool  m_GameStarted = false;
		int m_FrameCount = 0;

		Clock::duration m_FrameDuration = std::chrono::duration_cast<Clock::duration>(
			std::chrono::duration<float>(1.0f / m_TargetFPS)
		);
		Clock::time_point m_StartTime = Clock::now();
		Clock::time_point m_GameStartTime = Clock::now();

		friend class Application;
		friend class ApplicationEditorAccess;
	};
}
