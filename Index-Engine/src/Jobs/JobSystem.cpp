#include "pch.hpp"

#include "Jobs/JobSystem.hpp"

#include "Profiling/Profiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

#include <blockingconcurrentqueue.h>

#ifdef IDX_PLATFORM_WINDOWS
#include <processthreadsapi.h>
#endif

namespace Index {

	namespace {

		// MPMC lock-free queue; Enqueue wait-free in common path. Replaces prior mutex+deque+condvar that serialized all operations and thundering-herded workers.
		moodycamel::BlockingConcurrentQueue<std::function<void()>> s_Queue;

		// Single PoolState atomic: previously two independent atomics let Shutdown flip Running between the Enqueue load and the actual insert, stranding jobs.
		enum class PoolState : uint8_t { Uninit = 0, Running = 1, ShuttingDown = 2 };
		std::atomic<PoolState>           s_State{ PoolState::Uninit };
		std::vector<std::thread>         s_Workers;

		thread_local int                 t_WorkerIndex = -1;

		int ResolveAutoWorkerCount(int requested) {
			if (requested > 0) {
				return std::min(requested, 32);
			}

			unsigned int hw = std::thread::hardware_concurrency();
			if (hw == 0) hw = 4; // hardware_concurrency may return 0 on unusual platforms.

			int autoCount = static_cast<int>(hw) - 1;
#if INDEX_WITH_SCRIPTING
			// Cap at hw-2: leaves headroom for the .NET ThreadPool (Index.Jobs) and main thread on 4-core hosts.
			autoCount = std::min(autoCount, static_cast<int>(hw) - 2);
#endif
			if (autoCount < 2)  autoCount = 2;
			if (autoCount > 16) autoCount = 16;
			return autoCount;
		}

		void SetWorkerThreadName(int workerIndex) {
			char name[32];
			std::snprintf(name, sizeof(name), "Index Worker %d", workerIndex);
#ifdef IDX_PLATFORM_WINDOWS
			wchar_t wname[32];
			swprintf_s(wname, L"Index Worker %d", workerIndex);
			::SetThreadDescription(::GetCurrentThread(), wname);
#endif
			INDEX_PROFILE_THREAD_NAME(name);
		}

		void ExecuteJob(std::function<void()>&& job) {
			INDEX_PROFILE_SCOPE("JobSystem::Job");
			try {
				if (job) job();
			}
			catch (const std::exception& e) {
				IDX_CORE_ERROR_TAG("JobSystem", "Job failed: {}", e.what());
			}
			catch (...) {
				IDX_CORE_ERROR_TAG("JobSystem", "Job failed: unknown exception");
			}
		}

		void WorkerMain(int workerIndex) {
			t_WorkerIndex = workerIndex;
			SetWorkerThreadName(workerIndex);

			std::function<void()> job;
			while (s_State.load(std::memory_order_acquire) == PoolState::Running) {
				if (s_Queue.wait_dequeue_timed(job, std::chrono::milliseconds(50))) {
					ExecuteJob(std::move(job));
				}
			}

			while (s_Queue.try_dequeue(job)) {
				ExecuteJob(std::move(job));
			}
		}

	} // namespace

	void JobSystem::Initialize(const JobSystemSpec& spec) {
		if (s_State.load(std::memory_order_acquire) != PoolState::Uninit) {
			IDX_CORE_WARN_TAG("JobSystem", "Initialize called twice — ignoring second call");
			return;
		}

		const int workerCount = ResolveAutoWorkerCount(spec.WorkerCount);

		s_Workers.reserve(static_cast<size_t>(workerCount));
		// Flip state to Running BEFORE spawning workers so the first thing
		// each WorkerMain sees is the live state, not Uninit.
		s_State.store(PoolState::Running, std::memory_order_release);
		for (int i = 0; i < workerCount; ++i) {
			s_Workers.emplace_back(WorkerMain, i);
		}

		IDX_CORE_INFO_TAG("JobSystem", "Started with {} worker thread(s)", workerCount);
	}

	void JobSystem::Shutdown() {
		if (s_State.load(std::memory_order_acquire) != PoolState::Running) {
			return;
		}

		s_State.store(PoolState::ShuttingDown, std::memory_order_release);

		// Poison-pill wake: N empty jobs unblock workers so the Running check fires promptly; genuine work that slipped in is drained by WorkerMain's post-loop.
		const size_t workerCount = s_Workers.size();
		for (size_t i = 0; i < workerCount; ++i) {
			s_Queue.enqueue(std::function<void()>{});
		}

		for (auto& worker : s_Workers) {
			if (worker.joinable()) {
				worker.join();
			}
		}
		s_Workers.clear();

		std::function<void()> sink;
		while (s_Queue.try_dequeue(sink)) { /* discard */ }

		s_State.store(PoolState::Uninit, std::memory_order_release);
		IDX_CORE_INFO_TAG("JobSystem", "Stopped");
	}

	int JobSystem::Reconfigure(int workerCount) {
		Shutdown();
		JobSystemSpec spec;
		spec.WorkerCount = workerCount;
		Initialize(spec);
		return GetWorkerCount();
	}

	bool JobSystem::IsInitialized() {
		return s_State.load(std::memory_order_acquire) == PoolState::Running;
	}

	int JobSystem::GetWorkerCount() {
		return static_cast<int>(s_Workers.size());
	}

	bool JobSystem::IsCallerWorker() {
		return t_WorkerIndex >= 0;
	}

	int JobSystem::GetWorkerIndex() {
		return t_WorkerIndex;
	}

	void JobSystem::Enqueue(std::function<void()> work) {
		if (!work) return;

		// Run inline if the pool is not up; the second state check below drains anything that raced with Shutdown.
		if (s_State.load(std::memory_order_acquire) != PoolState::Running) {
			ExecuteJob(std::move(work));
			return;
		}
		s_Queue.enqueue(std::move(work));

		// Re-check: Shutdown may have flipped after the first load; workers' drain loop may have already exited, so drain inline here to avoid losing the enqueue.
		if (s_State.load(std::memory_order_acquire) != PoolState::Running) {
			std::function<void()> drained;
			while (s_Queue.try_dequeue(drained)) {
				if (drained) ExecuteJob(std::move(drained));
			}
		}
	}

	bool JobSystem::TryPopAndRun() {
		std::function<void()> job;
		if (!s_Queue.try_dequeue(job)) {
			return false;
		}
		ExecuteJob(std::move(job));
		return true;
	}

} // namespace Index
