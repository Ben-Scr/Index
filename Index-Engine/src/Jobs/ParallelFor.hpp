#pragma once

#include "Jobs/Job.hpp"
#include "Jobs/JobSystem.hpp"

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace Index {

	INDEX_API size_t ComputeAutoGrainSize(size_t rangeLength, int workerCount);

	namespace Detail {

		template <typename F>
		inline void InvokeParallelChunk(F& fn, size_t lo, size_t hi) {
			if constexpr (std::is_invocable_v<F&, size_t, size_t>) {
				fn(lo, hi);
			}
			else if constexpr (std::is_invocable_v<F&, size_t>) {
				for (size_t i = lo; i < hi; ++i) {
					fn(i);
				}
			}
			else {
				static_assert(sizeof(F) == 0,
					"ParallelFor body must be callable with (size_t) or (size_t, size_t)");
			}
		}

	} // namespace Detail

	template <typename F>
	JobHandle ParallelForAsync(size_t begin, size_t end, F&& fn, size_t grainSize = 0) {
		if (begin >= end) {
			return JobHandle{};
		}

		const int workers = JobSystem::GetWorkerCount();
		const size_t rangeLength = end - begin;

		if (grainSize == 0) {
			grainSize = ComputeAutoGrainSize(rangeLength, workers > 0 ? workers : 1);
		}
		if (grainSize == 0) {
			grainSize = 1;
		}

		const size_t numChunks = (rangeLength + grainSize - 1) / grainSize;

		// Degenerate cases (single chunk, or no pool available): run
		// through the normal job path. If the pool is unavailable,
		// JobSystem::Enqueue runs inline and still completes the handle.
		if (numChunks <= 1 || workers <= 0) {
			auto fnPtr = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
			auto block = JobInternal::CreateBlock(1);
			JobSystem::Enqueue([block, fnPtr, begin, end]() {
				JobInternal::ExecuteAndNotify(block, [&]() {
					Detail::InvokeParallelChunk(*fnPtr, begin, end);
				});
			});
			return JobHandle(std::move(block));
		}

		auto fnPtr = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
		auto block = JobInternal::CreateBlock(static_cast<int>(numChunks));

		for (size_t c = 0; c < numChunks; ++c) {
			const size_t lo = begin + c * grainSize;
			const size_t hi = (lo + grainSize < end) ? (lo + grainSize) : end;

			JobSystem::Enqueue([block, fnPtr, lo, hi]() {
				JobInternal::ExecuteAndNotify(block, [&]() {
					Detail::InvokeParallelChunk(*fnPtr, lo, hi);
				});
			});
		}

		return JobHandle(std::move(block));
	}

	template <typename F>
	void ParallelFor(size_t begin, size_t end, F&& fn, size_t grainSize = 0) {
		JobHandle handle = ParallelForAsync(begin, end, std::forward<F>(fn), grainSize);
		Job::Wait(handle);
	}

} // namespace Index
