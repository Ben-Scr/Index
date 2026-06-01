#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <utility>

namespace Index {

	struct JobControlBlock;

	// Default-constructed handle is invalid; IsComplete() returns true for it ("no job" = "already done").
	class JobHandle {
	public:
		JobHandle() = default;

		INDEX_API bool IsValid()    const;
		INDEX_API bool IsComplete() const;

		explicit JobHandle(std::shared_ptr<JobControlBlock> block) noexcept
			: m_Block(std::move(block)) {
		}

		const std::shared_ptr<JobControlBlock>& GetBlock() const noexcept { return m_Block; }

	private:
		std::shared_ptr<JobControlBlock> m_Block;
	};

	class INDEX_API Job {
	public:
		static JobHandle Schedule(std::function<void()> work);
		// Work-steals the queue while waiting, so nested sub-job waits cannot deadlock.
		static void Wait(const JobHandle& handle);

		// Non-blocking completion check. Cheap atomic load.
		static bool IsComplete(const JobHandle& handle);

	private:
		Job() = delete;
	};

	namespace JobInternal {

		INDEX_API std::shared_ptr<JobControlBlock> CreateBlock(int pending);
		INDEX_API void NotifyOne(const std::shared_ptr<JobControlBlock>& block);

		template <typename F>
		void ExecuteAndNotify(const std::shared_ptr<JobControlBlock>& block, F&& work) {
			struct CompletionGuard {
				const std::shared_ptr<JobControlBlock>& Block;

				~CompletionGuard() {
					NotifyOne(Block);
				}
			};

			CompletionGuard guard{ block };
			std::forward<F>(work)();
		}

	}

} // namespace Index
