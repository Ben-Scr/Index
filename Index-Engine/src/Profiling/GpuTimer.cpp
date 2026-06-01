#include "pch.hpp"
#include "Profiling/GpuTimer.hpp"

#ifdef INDEX_PROFILER_ENABLED

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Profiling/Profiler.hpp"

#include <cstring>
#include <string>

namespace Index {

	GpuTimer::GpuTimer() = default;
	GpuTimer::~GpuTimer() { Shutdown(); }

	void GpuTimer::Initialize() {
		if (m_Initialized) return;
		if (!WebGPUBackend::IsInitialized()) return;
		if (!WebGPUBackend::HasTimestampQuery()) return;

		wgpu::Device device = WebGPUBackend::GetDevice();
		if (!device) return;

		wgpu::QuerySetDescriptor qsDesc{};
		qsDesc.type  = wgpu::QueryType::Timestamp;
		qsDesc.count = static_cast<uint32_t>(k_QueriesPerFrame * k_RingDepth);
		qsDesc.label = "gpu-timer-queryset";
		m_QuerySet = device.CreateQuerySet(&qsDesc);
		if (!m_QuerySet) {
			IDX_CORE_WARN_TAG("GpuTimer",
				"CreateQuerySet(Timestamp, count={}) failed — GPU module stays at N/A",
				qsDesc.count);
			return;
		}

		wgpu::BufferDescriptor resolveDesc{};
		// k_ResolveSlotStride=256: WebGPU requires ResolveQuerySet's destinationOffset to be a multiple of 256.
		resolveDesc.size  = static_cast<uint64_t>(k_ResolveSlotStride * k_RingDepth);
		resolveDesc.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
		resolveDesc.label = "gpu-timer-resolve";
		m_ResolveBuffer = device.CreateBuffer(&resolveDesc);
		if (!m_ResolveBuffer) {
			IDX_CORE_WARN_TAG("GpuTimer", "Failed to create resolve buffer");
			m_QuerySet = nullptr;
			return;
		}

		wgpu::BufferDescriptor rbDesc{};
		rbDesc.size  = static_cast<uint64_t>(k_BytesPerFrame);
		rbDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
		std::string label;
		for (size_t i = 0; i < k_RingDepth; ++i) {
			label = "gpu-timer-readback-" + std::to_string(i);
			rbDesc.label = label.c_str();
			m_Slots[i].Readback = device.CreateBuffer(&rbDesc);
			m_Slots[i].State    = SlotState::Empty;
			if (!m_Slots[i].Readback) {
				IDX_CORE_WARN_TAG("GpuTimer", "Failed to create readback buffer {}", i);
				m_QuerySet      = nullptr;
				m_ResolveBuffer = nullptr;
				for (auto& s : m_Slots) s.Readback = nullptr;
				return;
			}
		}

		m_CurrentSlot       = 0;
		m_FrameWritesIssued = false;
		m_Initialized       = true;
	}

	void GpuTimer::Shutdown() {
		m_QuerySet      = nullptr;
		m_ResolveBuffer = nullptr;
		for (auto& s : m_Slots) {
			// Defensive unmap — a slot still in AwaitingMap at shutdown
			// would otherwise leak the Dawn-internal map state. Unmap is
			// a no-op on an unmapped buffer.
			if (s.Readback) {
				s.Readback.Unmap();
			}
			s.Readback = nullptr;
			s.State    = SlotState::Empty;
		}
		m_CurrentSlot       = 0;
		m_FrameWritesIssued = false;
		m_Initialized       = false;
	}

	GpuTimer::FrameSlots GpuTimer::BeginFrameWrites() {
		FrameSlots out;
		if (!m_Initialized) return out;
		if (m_Slots[m_CurrentSlot].State != SlotState::Empty) {
			return out;
		}
		out.QuerySet                  = m_QuerySet;
		out.BeginningOfPassWriteIndex = static_cast<uint32_t>(m_CurrentSlot * k_QueriesPerFrame);
		out.EndOfPassWriteIndex       = static_cast<uint32_t>(m_CurrentSlot * k_QueriesPerFrame + 1);
		out.Valid                     = true;
		m_FrameWritesIssued           = true;
		return out;
	}

	void GpuTimer::ResolveCurrentFrame(wgpu::CommandEncoder encoder) {
		if (!m_Initialized || !m_FrameWritesIssued || !encoder) return;

		const uint32_t firstQuery    = static_cast<uint32_t>(m_CurrentSlot * k_QueriesPerFrame);
		// MUST use k_ResolveSlotStride (256), not payload size (16) — Dawn validates destinationOffset%256==0.
		const uint64_t resolveOffset = static_cast<uint64_t>(m_CurrentSlot * k_ResolveSlotStride);
		encoder.ResolveQuerySet(m_QuerySet,
			firstQuery, static_cast<uint32_t>(k_QueriesPerFrame),
			m_ResolveBuffer, resolveOffset);
		encoder.CopyBufferToBuffer(m_ResolveBuffer, resolveOffset,
			m_Slots[m_CurrentSlot].Readback, 0,
			static_cast<uint64_t>(k_BytesPerFrame));

		// MapAsync deferred to OnFrameStart(): calling it here transitions readback to Pending before
		// Queue::Submit, causing Dawn "used in submit while mapped" validation failure on the whole frame.
		m_Slots[m_CurrentSlot].State = SlotState::AwaitingSubmit;

		m_FrameWritesIssued = false;
		m_CurrentSlot       = (m_CurrentSlot + 1) % k_RingDepth;
	}

	void GpuTimer::OnFrameStart() {
		if (!m_Initialized) return;
		for (auto& s : m_Slots) {
			if (s.State != SlotState::AwaitingSubmit) continue;
			if (!s.Readback) continue;
			s.Readback.MapAsync(
				wgpu::MapMode::Read, 0,
				static_cast<size_t>(k_BytesPerFrame),
				wgpu::CallbackMode::AllowSpontaneous,
				[](wgpu::MapAsyncStatus /*status*/, wgpu::StringView /*msg*/) {});
			s.State = SlotState::AwaitingMap;
		}
	}

	void GpuTimer::PollAndPublish() {
		if (!m_Initialized) return;

		for (auto& s : m_Slots) {
			if (s.State != SlotState::AwaitingMap) continue;
			if (!s.Readback) continue;
			if (s.Readback.GetMapState() != wgpu::BufferMapState::Mapped) continue;

			const void* mapped = s.Readback.GetConstMappedRange(0, k_BytesPerFrame);
			if (mapped) {
				uint64_t ts[2] = { 0, 0 };
				std::memcpy(ts, mapped, sizeof(ts));
				const uint64_t deltaNs = ts[1] > ts[0] ? ts[1] - ts[0] : 0;
				const float    deltaMs = static_cast<float>(deltaNs) * 1.0e-6f;
				Profiler::PushSample("GPU", deltaMs);
				TracyPlot("GPU.ms", double(deltaMs));
			}
			s.Readback.Unmap();
			s.State = SlotState::Empty;
		}
	}

	long long GpuTimer::QueryGpuMemoryMb() {
		return -1;
	}

}  // namespace Index

#else  // !INDEX_PROFILER_ENABLED
#endif  // INDEX_PROFILER_ENABLED
