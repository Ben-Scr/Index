#include <doctest/doctest.h>

#include "Index/Core.hpp"

#include <atomic>
#include <stdexcept>

using namespace Index;

namespace {
	struct ScopedJobSystem {
		ScopedJobSystem() {
			JobSystem::Shutdown();

			JobSystemSpec spec;
			spec.WorkerCount = 2;
			JobSystem::Initialize(spec);
		}

		~ScopedJobSystem() {
			JobSystem::Shutdown();
		}
	};
}

TEST_CASE("Job API schedules work and completes handles") {
	ScopedJobSystem jobs;

	std::atomic<int> value{ 0 };
	JobHandle handle = Job::Schedule([&]() {
		value.store(42, std::memory_order_release);
	});

	CHECK(handle.IsValid());

	Job::Wait(handle);

	CHECK(Job::IsComplete(handle));
	CHECK(value.load(std::memory_order_acquire) == 42);
}

TEST_CASE("Job API completes handles when work throws") {
	ScopedJobSystem jobs;

	JobHandle handle = Job::Schedule([]() {
		throw std::runtime_error("expected test failure");
	});

	Job::Wait(handle);
	CHECK(handle.IsComplete());
}

TEST_CASE("ParallelFor handles complete when a chunk throws") {
	ScopedJobSystem jobs;

	JobHandle handle = ParallelForAsync(0, 64, [](size_t i) {
		if (i == 13) {
			throw std::runtime_error("expected chunk failure");
		}
	}, 1);

	Job::Wait(handle);
	CHECK(handle.IsComplete());
}

TEST_CASE("Nested jobs can wait without starving the worker queue") {
	ScopedJobSystem jobs;

	std::atomic<int> visits{ 0 };
	JobHandle outer = Job::Schedule([&]() {
		ParallelFor(0, 128, [&](size_t) {
			visits.fetch_add(1, std::memory_order_acq_rel);
		}, 1);
	});

	Job::Wait(outer);

	CHECK(outer.IsComplete());
	CHECK(visits.load(std::memory_order_acquire) == 128);
}
