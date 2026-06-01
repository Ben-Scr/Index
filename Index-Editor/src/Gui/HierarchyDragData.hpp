#pragma once
#include <cstdint>

namespace Index {

	// Layout MUST stay stable (binary-compatible across all producer/consumer sites). SourceSceneId is raw uint64 because UUID is not POD and ImGui payloads memcpy a fixed buffer.
	struct HierarchyDragData {
		int Index;
		uint32_t EntityHandle;
		uint64_t SourceSceneId;
	};

	struct HierarchySceneDragData {
		uint64_t SceneId;
	};

}
