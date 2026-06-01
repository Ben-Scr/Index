#pragma once

#include "Core/Export.hpp"
#include "Scene/EntityHandle.hpp"
#include "Scripting/NativeEntityCommandBuffer.hpp"

#include <vector>

namespace Index {

	class Scene;

	class INDEX_API ParallelEntityCommandBuffer {
	public:
		ParallelEntityCommandBuffer();

		ParallelEntityCommandBuffer(const ParallelEntityCommandBuffer&) = delete;
		ParallelEntityCommandBuffer& operator=(const ParallelEntityCommandBuffer&) = delete;
		ParallelEntityCommandBuffer(ParallelEntityCommandBuffer&&) noexcept = default;
		ParallelEntityCommandBuffer& operator=(ParallelEntityCommandBuffer&&) noexcept = default;

		NativeEntityCommandBuffer& AsWriter();
		int Playback(Scene& scene);
		void Clear();
		int EntityCount() const;
		int CommandCount() const;
		EntityHandle GetCreatedEntity(int i) const;

	private:
		std::vector<NativeEntityCommandBuffer> m_Subs;
		std::vector<EntityHandle>              m_CreatedHandles;
	};

} // namespace Index
