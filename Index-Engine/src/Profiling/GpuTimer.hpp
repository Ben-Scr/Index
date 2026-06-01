#pragma once

#include "Core/Export.hpp"

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Index {

	// GPU timer using wgpu timestamp queries (3-slot ring to avoid MapAsync stalls).
	// Only active when INDEX_PROFILER_ENABLED and TimestampQuery is available.
	class INDEX_API GpuTimer {
	public:
		GpuTimer();
		~GpuTimer();

		// Allocates the QuerySet + resolve + readback buffers when the
		// WebGPU device was created with TimestampQuery enabled. No-op
		// otherwise. Must run after the WebGPU device exists.
		void Initialize();

		// Releases all GPU resources. Safe to call before any Initialize.
		void Shutdown();

		// Slot info handed to the renderer so it can attach a
		// wgpu::RenderPassTimestampWrites to its pass descriptor. The
		// QuerySet field stays non-null only when Valid is true.
		struct FrameSlots {
			wgpu::QuerySet QuerySet;
			uint32_t       BeginningOfPassWriteIndex = 0;
			uint32_t       EndOfPassWriteIndex       = 0;
			bool           Valid = false;
		};

		FrameSlots BeginFrameWrites();

		// MapAsync deferred to OnFrameStart(): issuing it here races with the pending Submit and Dawn rejects 'used in submit while mapped'.
		void ResolveCurrentFrame(wgpu::CommandEncoder encoder);

		void OnFrameStart();

		// Polls any pending readbacks and publishes completed deltas into
		// the "GPU" profiler module (milliseconds). Non-blocking. Call once
		// per frame from outside any render pass.
		void PollAndPublish();

		// Returns -1 when no driver extension is available. WebGPU has no
		// portable surface for GPU memory queries today.
		static long long QueryGpuMemoryMb();

	private:
		static constexpr size_t k_RingDepth        = 3;
		static constexpr size_t k_QueriesPerFrame  = 2;          // begin + end
		static constexpr size_t k_BytesPerQuery    = 8;          // u64 timestamp
		static constexpr size_t k_BytesPerFrame    = k_QueriesPerFrame * k_BytesPerQuery; // 16
		// 256-byte stride: WebGPU requires ResolveQuerySet dest offset to be 256-aligned; each slot wastes 240 bytes of padding.
		static constexpr size_t k_ResolveSlotStride = 256;

		enum class SlotState : uint8_t {
			Empty,           // Available for the next frame's resolve
			AwaitingSubmit,  // Copy encoded into this frame's cmd buffer; MapAsync deferred
			AwaitingMap,     // MapAsync issued; PollAndPublish drains on completion
		};

		struct Slot {
			wgpu::Buffer Readback;
			SlotState    State = SlotState::Empty;
		};

		wgpu::QuerySet                m_QuerySet;
		wgpu::Buffer                  m_ResolveBuffer;  // size = k_BytesPerFrame * k_RingDepth
		std::array<Slot, k_RingDepth> m_Slots{};
		size_t                        m_CurrentSlot = 0;
		bool                          m_Initialized = false;
		bool                          m_FrameWritesIssued = false;
	};

} // namespace Index
