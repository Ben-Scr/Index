#pragma once
#include <imgui.h>

namespace Index::EditorTheme {

	namespace Colors {

		// Prefab instance whose source GUID resolves in the AssetRegistry.
		inline constexpr ImVec4 PrefabInstance = ImVec4(0.42f, 0.66f, 0.95f, 1.00f);

		// Placeholder for future entity hierarchy — unused until parent/child hierarchy lands.
		inline constexpr ImVec4 PrefabChild = ImVec4(0.55f, 0.70f, 0.85f, 0.85f);

		// Unresolvable source GUID (deleted file/missing metadata); apply/revert unavailable.
		inline constexpr ImVec4 PrefabOrphan = ImVec4(0.95f, 0.70f, 0.30f, 1.00f);

		// Override marker dot drawn next to a component header when any
		// field of that component differs from the source prefab.
		inline constexpr ImVec4 OverrideMarker = ImVec4(0.42f, 0.66f, 0.95f, 1.00f);

		// Asset Browser tile selection. Kept neutral so file selection feels
		// like part of the editor chrome instead of the blue accent layer.
		inline constexpr ImVec4 AssetTileSelection = ImVec4(0.25f, 0.25f, 0.29f, 0.82f);
		inline constexpr ImVec4 AssetTileSelectionBorder = ImVec4(0.43f, 0.43f, 0.49f, 0.75f);
	}
}
