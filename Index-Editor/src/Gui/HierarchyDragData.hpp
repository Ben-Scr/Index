#pragma once
#include <cstdint>

namespace Index {

	// Drag-and-drop payload for the "HIERARCHY_ENTITY" payload type.
	// Used when dragging entities within the Hierarchy panel (reorder)
	// or onto external drop targets such as the Asset Browser
	// (turn an entity into a prefab) or property inspectors
	// (assign an entity reference). Layout MUST stay stable so all
	// producer/consumer call sites stay binary-compatible.
	//
	// `SourceSceneId` carries the originating scene's UUID so a drop
	// target with multiple scenes loaded (the Entities panel) can tell
	// whether the gesture is a same-scene reorder or a cross-scene
	// migration. Stored as raw uint64 because UUID is not a POD-friendly
	// type and ImGui drag payloads memcpy a fixed byte buffer.
	struct HierarchyDragData {
		int Index;
		uint32_t EntityHandle;
		uint64_t SourceSceneId;
	};

	// Payload for the "HIERARCHY_SCENE" type — produced when the user
	// starts a drag from a scene's tree-node header in the Entities
	// panel. Reordering scenes only affects the SceneManager's loaded
	// list (display order + iteration order), so the payload only needs
	// to identify which scene moved; the drop target derives the new
	// position from the drop site itself.
	struct HierarchySceneDragData {
		uint64_t SceneId;
	};

}
