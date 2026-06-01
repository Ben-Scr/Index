#pragma once

#include "Core/Export.hpp"

#ifdef INDEX_PROFILER_ENABLED
// MSVC: __FUNCTION__ is not a string literal, so Tracy's constexpr SourceLocationData fails; override to "" to satisfy constexpr and avoid strlen(nullptr) UB. Must be before <tracy/Tracy.hpp>.
#  ifndef TracyFunction
#    define TracyFunction ""
#  endif
#  include <tracy/Tracy.hpp>
#endif

#include <cstddef>
#include <string>
#include <vector>
#include <chrono>

namespace Index {

	struct INDEX_API ProfilerModule {
		std::string Name;
		bool Enabled = true;     // panel checkbox
		bool Available = true;   // GPU memory may flip this off when no driver ext

		std::vector<float> Samples;
		size_t Head = 0;
		size_t Count = 0;

		float CurrentValue = 0.0f;
		float AvgValue = 0.0f;
		float MinValue = 0.0f;
		float MaxValue = 0.0f;

		double RunningSum = 0.0; // cached so Avg is O(1)
		// Per-module gate; previously a single shared timestamp that starved every module except PushFrameDelta each tick.
		std::chrono::steady_clock::time_point LastPushTime{};
	};

#ifdef INDEX_PROFILER_ENABLED
	class INDEX_API Profiler {
	public:
		// Called once at engine init. Registers the eight built-in modules
		// (idempotent — repeated calls clear and re-register).
		static void Initialize();

		// Called at engine shutdown. Drops all modules + clears state.
		static void Shutdown();

		// Module registration. Idempotent: registering an existing name
		// just returns the existing module (lets call sites be defensive).
		static ProfilerModule* Register(const std::string& name);

		// Lookups. nullptr if no such module.
		static ProfilerModule* Find(const std::string& name);
		// Returns by value: each call deep-copies every ring buffer. Prefer the
		// buffer-fill overload below in hot consumers (editor render loop).
		static std::vector<ProfilerModule> AllModules();
		// Reuse the caller's buffer to avoid the per-frame allocations the
		// by-value overload incurs.
		static void AllModules(std::vector<ProfilerModule>& outBuffer);

		// Returns 0.0 if no such module; used by the "Others" residual in Application::Run for unattributed CPU time.
		static float GetCurrentValue(const std::string& name);

		// Push an already-computed numeric sample into a named module.
		// Used for non-time metrics (DrawCalls, Entities, RAM, GPU memory).
		// Honors the visibility / enabled / sampling-rate gates.
		static void PushValue(const std::string& name, float value);

		static void PushSample(const std::string& name, float milliseconds);

		// OnFrameMark advances the sampling cadence; PushFrameDelta records FPS + FrameTime from one shared dt so they can't drift.
		static void OnFrameMark(const std::string& name);
		static void PushFrameDelta(float deltaSeconds);

		// Both gates required for samples to land; background tracking keeps collection alive when the panel is closed.
		static void SetPanelVisible(bool visible);
		static void SetBackgroundTracking(bool enabled);
		static bool IsCollecting();

		// Sampling configuration. Both persisted by the editor across sessions.
		static void SetSamplingHz(int hz);
		static void SetTrackingSpan(int span);
		static int  GetSamplingHz();
		static int  GetTrackingSpan();

		// Per-module enable flag. Disabling clears the module's ring buffer.
		static void SetModuleEnabled(const std::string& name, bool enabled);

	private:
		Profiler() = delete;
	};
#else
	// Stripped-build stubs — all no-ops; safe defaults so callers branching on return values still work.
	class Profiler {
	public:
		static inline void Initialize() {}
		static inline void Shutdown() {}
		static inline ProfilerModule* Register(const std::string&) { return nullptr; }
		static inline ProfilerModule* Find(const std::string&) { return nullptr; }
		static inline std::vector<ProfilerModule> AllModules() { return {}; }
		static inline void AllModules(std::vector<ProfilerModule>& outBuffer) { outBuffer.clear(); }
		static inline float GetCurrentValue(const std::string&) { return 0.0f; }
		static inline void PushValue(const std::string&, float) {}
		static inline void PushSample(const std::string&, float) {}
		static inline void OnFrameMark(const std::string&) {}
		static inline void PushFrameDelta(float) {}
		static inline void SetPanelVisible(bool) {}
		static inline void SetBackgroundTracking(bool) {}
		static inline bool IsCollecting() { return false; }
		static inline void SetSamplingHz(int) {}
		static inline void SetTrackingSpan(int) {}
		static inline int  GetSamplingHz() { return 60; }
		static inline int  GetTrackingSpan() { return 200; }
		static inline void SetModuleEnabled(const std::string&, bool) {}
	private:
		Profiler() = delete;
	};
#endif

	// ── Macro plumbing ────────────────────────────────────────────────────

#ifdef INDEX_PROFILER_ENABLED

	class INDEX_API ProfilerScopedSample {
	public:
		explicit ProfilerScopedSample(const char* name)
			: m_Name(name)
			, m_Start(std::chrono::steady_clock::now()) {
		}
		~ProfilerScopedSample() {
			using namespace std::chrono;
			const auto end = steady_clock::now();
			const float ms = duration<float, std::milli>(end - m_Start).count();
			Profiler::PushSample(m_Name, ms);
		}
		ProfilerScopedSample(const ProfilerScopedSample&) = delete;
		ProfilerScopedSample& operator=(const ProfilerScopedSample&) = delete;
	private:
		const char* m_Name;
		std::chrono::steady_clock::time_point m_Start;
	};

	// Token-pasting helper so two macros expanded on the same source line
	// don't clash on the local variable name.
	#define IDX_PROF_CAT_INNER(a, b) a##b
	#define IDX_PROF_CAT(a, b) IDX_PROF_CAT_INNER(a, b)

	// Uses ZoneTransientN (runtime string) because MSVC's __FUNCTION__ is not a string literal and breaks constexpr SourceLocationData.
	#define INDEX_PROFILE_SCOPE(name)                                       \
		ZoneTransientN(IDX_PROF_CAT(_idx_zone_, __LINE__), name, true);     \
		::Index::ProfilerScopedSample IDX_PROF_CAT(_idx_prof_, __LINE__)(name)

	// Mark end-of-frame. Tracy frame mark + sampling cadence advance.
	#define INDEX_PROFILE_FRAME(name)                                       \
		FrameMarkNamed(name);                                               \
		::Index::Profiler::OnFrameMark(name)

	// Push a non-time numeric value (draw calls, entity count, RAM MB).
	// Tracy plot + ring-buffer push.
	#define INDEX_PROFILE_VALUE(name, val)                                  \
		do {                                                                \
			TracyPlot(name, double(val));                                   \
			::Index::Profiler::PushValue(name, float(val));                 \
		} while (0)

	#define INDEX_PROFILE_THREAD_NAME(name) ::tracy::SetThreadName(name)

#else // !INDEX_PROFILER_ENABLED — strip everything

	#define INDEX_PROFILE_SCOPE(name)       ((void)0)
	#define INDEX_PROFILE_FRAME(name)       ((void)0)
	#define INDEX_PROFILE_VALUE(name, val)  ((void)0)
	#define INDEX_PROFILE_THREAD_NAME(name) ((void)0)

#endif

} // namespace Index
