#include "pch.hpp"

#include "Jobs/Job.hpp"
#include "Jobs/JobSystem.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace Index {

	// Internal control block. Lives behind a shared_ptr so it outlives both
	// the worker thread that completes it and the caller that waits on it.
	struct JobControlBlock {
		std::atomic<int>          Pending{0};
		std::atomic<bool>         Done{false};
		std::mutex                Mutex;
		std::condition_variable   Cv;
	};

	namespace JobInternal {

		std::shared_ptr<JobControlBlock> CreateBlock(int pending) {
			auto block = std::make_shared<JobControlBlock>();
			block->Pending.store(pending, std::memory_order_release);
			return block;
		}

		void NotifyOne(const std::shared_ptr<JobControlBlock>& block) {
			if (!block) return;

			const int remaining = block->Pending.fetch_sub(1, std::memory_order_acq_rel) - 1;
			if (remaining > 0) {
				return;
			}

			{
				std::scoped_lock lock(block->Mutex);
				block->Done.store(true, std::memory_order_release);
			}
			block->Cv.notify_all();
		}

	} // namespace JobInternal

	// ── JobHandle ────────────────────────────────────────────────────

	bool JobHandle::IsValid() const {
		return static_cast<bool>(m_Block);
	}

	bool JobHandle::IsComplete() const {
		// A default-constructed handle counts as complete — "nothing to do"
		// reads naturally as done.
		if (!m_Block) return true;
		return m_Block->Done.load(std::memory_order_acquire);
	}

	// ── Job ──────────────────────────────────────────────────────────

	JobHandle Job::Schedule(std::function<void()> work) {
		auto block = JobInternal::CreateBlock(1);

		JobSystem::Enqueue([block, work = std::move(work)]() mutable {
			JobInternal::ExecuteAndNotify(block, [&]() {
				if (work) work();
			});
		});

		return JobHandle(std::move(block));
	}

	void Job::Wait(const JobHandle& handle) {
		const auto& block = handle.GetBlock();
		if (!block) return;

		// Fast path: already done.
		if (block->Done.load(std::memory_order_acquire)) return;

		// Drain the queue while waiting: blocking here with queued work risks fork-join deadlock when all workers wait on each other.
		using namespace std::chrono_literals;
		for (;;) {
			if (block->Done.load(std::memory_order_acquire)) return;

			if (JobSystem::TryPopAndRun()) {
				continue;
			}

			// Queue is empty but our handle isn't done yet. Brief
			// timed-wait on the completion cv so we don't burn a core,
			// then re-check (in case a job is enqueued during the wait).
			std::unique_lock<std::mutex> lock(block->Mutex);
			block->Cv.wait_for(lock, 1ms, [&] {
				return block->Done.load(std::memory_order_acquire);
			});
			if (block->Done.load(std::memory_order_acquire)) return;
		}
	}

	bool Job::IsComplete(const JobHandle& handle) {
		return handle.IsComplete();
	}

} // namespace Index
