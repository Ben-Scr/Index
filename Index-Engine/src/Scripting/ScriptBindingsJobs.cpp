#include "pch.hpp"

#include "Jobs/Job.hpp"
#include "Jobs/JobSystem.hpp"
#include "Jobs/ParallelFor.hpp"
#include "Scripting/ScriptGlue.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace Index {

	// Map from managed-visible u64 handle id -> live JobHandle + the
	// managed-supplied context + release callback. The map is the bridge
	// between the managed "handle is a stable u64 I can pass around" view
	// and the native "JobHandle wraps a shared_ptr to a control block"
	// view.
	//
	// Single mutex is adequate at job-schedule cadence (a few k/sec on
	// a busy frame, never on a per-iteration hot path). If contention
	// ever shows up here, switch to a sharded map or a
	// concurrent-hash-map; the binding contract is unchanged.
	struct JobBindingEntry {
		JobHandle handle;
		void* context;
		void (*releaseContext)(void* context);
	};

	struct JobBindingMap {
		std::mutex mtx;
		std::unordered_map<uint64_t, JobBindingEntry> entries;
		std::atomic<uint64_t> nextId{ 1 }; // 0 is reserved as "invalid"
	};

	static JobBindingMap& GetMap()
	{
		static JobBindingMap s;
		return s;
	}

	static uint64_t Index_JobSystem_Enqueue(
		void (*work)(void* context),
		void* context,
		void (*releaseContext)(void* context))
	{
		// Defensive — the managed dispatcher always passes a valid
		// callback, but a null work pointer is treated as a no-op so a
		// bug on either side doesn't crash the worker.
		if (work == nullptr) {
			// Caller still expects a handle they can Wait/Release on.
			JobHandle empty = Job::Schedule([]() {});
			JobBindingMap& m = GetMap();
			uint64_t id = m.nextId.fetch_add(1, std::memory_order_relaxed);
			{
				std::scoped_lock lock(m.mtx);
				m.entries.emplace(id, JobBindingEntry{ std::move(empty), context, releaseContext });
			}
			return id;
		}

		JobHandle handle = Job::Schedule([work, context]() {
			work(context);
		});

		JobBindingMap& m = GetMap();
		uint64_t id = m.nextId.fetch_add(1, std::memory_order_relaxed);
		{
			std::scoped_lock lock(m.mtx);
			m.entries.emplace(id, JobBindingEntry{ std::move(handle), context, releaseContext });
		}
		return id;
	}

	static uint64_t Index_JobSystem_ParallelFor(
		int begin, int end, int batchSize,
		void (*work)(void* context, int lo, int hi),
		void* context,
		void (*releaseContext)(void* context))
	{
		if (work == nullptr || begin >= end) {
			// Same defensive shape as Enqueue — return an empty handle so
			// the managed side can still Wait/Release without branching.
			JobHandle empty = Job::Schedule([]() {});
			JobBindingMap& m = GetMap();
			uint64_t id = m.nextId.fetch_add(1, std::memory_order_relaxed);
			{
				std::scoped_lock lock(m.mtx);
				m.entries.emplace(id, JobBindingEntry{ std::move(empty), context, releaseContext });
			}
			return id;
		}

		// The native ParallelForAsync expects the body signature
		// fn(size_t lo, size_t hi). Wrap to bridge int <-> size_t.
		JobHandle handle = ParallelForAsync(
			static_cast<size_t>(begin),
			static_cast<size_t>(end),
			[work, context](size_t lo, size_t hi) {
				work(context, static_cast<int>(lo), static_cast<int>(hi));
			},
			static_cast<size_t>(batchSize < 0 ? 0 : batchSize));

		JobBindingMap& m = GetMap();
		uint64_t id = m.nextId.fetch_add(1, std::memory_order_relaxed);
		{
			std::scoped_lock lock(m.mtx);
			m.entries.emplace(id, JobBindingEntry{ std::move(handle), context, releaseContext });
		}
		return id;
	}

	static void Index_JobSystem_Wait(uint64_t handleId)
	{
		if (handleId == 0) return;

		// Snapshot the handle under the lock so we don't hold the map
		// mutex across Job::Wait (which can block + work-steal for a
		// while). Copying a JobHandle is cheap (shared_ptr increment).
		JobHandle h;
		{
			JobBindingMap& m = GetMap();
			std::scoped_lock lock(m.mtx);
			auto it = m.entries.find(handleId);
			if (it == m.entries.end()) return;
			h = it->second.handle;
		}
		Job::Wait(h);
	}

	static int Index_JobSystem_IsComplete(uint64_t handleId)
	{
		if (handleId == 0) return 1;

		JobBindingMap& m = GetMap();
		std::scoped_lock lock(m.mtx);
		auto it = m.entries.find(handleId);
		if (it == m.entries.end()) return 1;
		return it->second.handle.IsComplete() ? 1 : 0;
	}

	static void Index_JobSystem_Release(uint64_t handleId)
	{
		if (handleId == 0) return;

		// Pull the entry out under the lock so we can call the managed
		// release callback OUTSIDE the lock — the callback frees a
		// GCHandle which may trigger arbitrary managed work, and we
		// don't want to hold the binding-map mutex during that.
		JobBindingEntry entry;
		{
			JobBindingMap& m = GetMap();
			std::scoped_lock lock(m.mtx);
			auto it = m.entries.find(handleId);
			if (it == m.entries.end()) return;
			entry = std::move(it->second);
			m.entries.erase(it);
		}
		if (entry.releaseContext != nullptr) {
			entry.releaseContext(entry.context);
		}
		// JobHandle in `entry` falls out of scope here, dropping its
		// shared_ptr reference to the JobControlBlock. The block frees
		// itself if no waiters retained a copy.
	}

	static int Index_JobSystem_GetWorkerCount()
	{
		return JobSystem::GetWorkerCount();
	}

	static int Index_JobSystem_GetCallerWorkerIndex()
	{
		return JobSystem::GetWorkerIndex();
	}

	void PopulateJobsBindings(NativeBindings& b)
	{
		b.JobSystem_Enqueue              = &Index_JobSystem_Enqueue;
		b.JobSystem_ParallelFor          = &Index_JobSystem_ParallelFor;
		b.JobSystem_Wait                 = &Index_JobSystem_Wait;
		b.JobSystem_IsComplete           = &Index_JobSystem_IsComplete;
		b.JobSystem_Release              = &Index_JobSystem_Release;
		b.JobSystem_GetWorkerCount       = &Index_JobSystem_GetWorkerCount;
		b.JobSystem_GetCallerWorkerIndex = &Index_JobSystem_GetCallerWorkerIndex;
	}

} // namespace Index
