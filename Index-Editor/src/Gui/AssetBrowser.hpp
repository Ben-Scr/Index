#pragma once
#include "Assets/TextureMeta.hpp"
#include "Gui/ThumbnailCache.hpp"
#include "Scene/EntityHandle.hpp"
#include "Serialization/Directory.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Index {

	class AssetBrowser {
	public:
		void Initialize(const std::string& rootDirectory);
		void Shutdown();
		void Render();
		void RequestRefresh() { m_NeedsRefresh = true; }
		static void RequestRevealAsset(const std::string& path);

		// Drop every cached thumbnail. Called by the editor's hot-reload
		// callback after image files on disk change so the asset-tile
		// previews re-fetch from the updated files on the next frame.
		void InvalidateAllThumbnails() { m_Thumbnails.Clear(); }

		/// Returns true when the user is currently naming a new script (rename in progress).
		bool IsCreatingScript() const { return m_PendingScriptType != PendingScriptType::None; }
		bool BeginRenameSelected();

		std::string TakePendingSceneLoad() {
			std::string p = std::move(m_PendingSceneLoad);
			m_PendingSceneLoad.clear();
			return p;
		}

		std::string TakePendingPrefabEdit() {
			std::string p = std::move(m_PendingPrefabEdit);
			m_PendingPrefabEdit.clear();
			return p;
		}

		/// Call this when external files are dropped onto the window.
		void OnExternalFileDrop(const std::vector<std::string>& paths);

		/// Returns the currently selected asset path (empty if none).
		const std::string& GetSelectedPath() const { return m_SelectedPath; }
		void ClearSelection() {
			ClearAssetSelection();
		}
		bool TakeSelectionActivated() {
			const bool activated = m_SelectionActivated;
			m_SelectionActivated = false;
			return activated;
		}

	private:
		void NavigateTo(const std::string& directory);
		void NavigateUp();
		void Refresh();

		void RenderBreadcrumb();
		void RenderSearchBar();
		void RebuildSearchResults();
		void RenderGrid();
		void RenderAssetTile(const DirectoryEntry& entry, int index);

		void RenderGridContextMenu();
		void RenderItemContextMenu(const DirectoryEntry& entry, int index);

		void HandleDragSource(const DirectoryEntry& entry);
		void HandleDropTarget(const DirectoryEntry& entry);
#ifdef IDX_PLATFORM_WINDOWS
		void PrepareExternalFileDrag(const DirectoryEntry& entry);
		void ClearExternalFileDrag();
		void MaybeStartExternalFileDrag();
#endif

		// Emits ASSET_SPRITE_SLICE drag payload (not a plain texture drag) so drop targets can distinguish slice vs full-texture drops.
		void RenderSliceTile(const DirectoryEntry& parentEntry,
			const SpriteSlice& slice, int sliceIndex, int tileIndex);

		void RebuildSliceCache();

		void CreateAssetWithCollisionCheck(
			std::filesystem::path preferredPath,
			std::function<void(const std::filesystem::path& finalPath)> doCreate);

		void RenderCreationCollisionPrompt();

		// Confirmation dialog before deleting selected assets, gated by
		// EditorPreferences::GetConfirmOnDelete().
		void RenderDeleteConfirmationPrompt();

		// .scene files embed their displayed name; a byte-copy via Duplicate /
		// Paste leaves it stale and the hierarchy keeps showing the source name.
		void SyncSceneEmbeddedNameIfNeeded(const std::filesystem::path& copiedPath);

		bool TryCreatePrefabFromHierarchyDrop(const struct ImGuiPayload* payload, const std::string& targetDirectory);
		void OpenAssetExternal(const DirectoryEntry& entry);
		void OpenAssetPath(const std::string& path);
		void RevealAssetInExplorer(const std::string& path);
		bool RevealAssetInBrowser(const std::string& path);

		void ClearAssetSelection();
		bool IsPathSelected(const std::string& path) const;
		bool IsPathInCutClipboard(const std::string& path) const;
		std::vector<std::string> GetSelectedPaths() const;
		void SetSingleSelection(const std::string& path, int index);
		void ToggleSelection(const std::string& path, int index);
		void SelectRange(int index);
		void HandleAssetShortcuts();
		void CopySelectedAssets(bool cut);
		void PasteAssets();
		void DuplicateSelectedAssets();
		// User-facing delete entry point: opens the confirmation dialog when
		// enabled in preferences, otherwise deletes immediately.
		void RequestDeleteSelectedAssets();
		void DeleteSelectedAssets();

		void DeleteEntry(const std::string& path);
		void RenameEntry(const std::string& path, const std::string& newName);
		void CopyPathToClipboard(const std::string& path);
		void CreateFolder(const std::string& parentDir);
		void CreateScript(const std::string& parentDir);
		void CreateManagedCSharpComponent(const std::string& parentDir);
		void CreateNativeCSharpComponent(const std::string& parentDir);
		void CreateSceneSystem(const std::string& parentDir);
		void CreateGlobalSystem(const std::string& parentDir);
		void CreateEmptyScript(const std::string& parentDir);
		void CreateDataAssetScript(const std::string& parentDir);
		void CreateScene(const std::string& parentDir);
		void CreateDataAsset(const std::string& parentDir, const std::string& typeName);
		void CreateEntityPrefab(const std::string& parentDir, EntityHandle sourceEntity = entt::null);
		void CreateDefaultTexture(const std::string& parentDir,
			const std::string& sourceFile, const std::string& displayName);
		void CreateFile(const std::string& parentDir, const std::string& baseName,
			const std::string& extension, const std::string& defaultContent);

		void BeginRename(const std::string& path, const std::string& currentName);
		void CommitRename();
		void CancelRename();
		bool IsRenamingEntry(const std::string& path) const;

		std::string m_RootDirectory;
		std::string m_CurrentDirectory;
		std::vector<DirectoryEntry> m_Entries;
		std::vector<std::string> m_VisibleEntryPaths;
		std::string m_SelectedPath;
		std::vector<std::string> m_SelectedPaths;
		int m_LastSelectionIndex = -1;
		std::string m_PressedPath;
		std::string m_PendingScrollToPath;
#ifdef IDX_PLATFORM_WINDOWS
		std::vector<std::string> m_ExternalDragPaths;
		int m_ExternalDragStartScreenX = 0;
		int m_ExternalDragStartScreenY = 0;
		void* m_ExternalDragCaptureWindow = nullptr;
#endif
		bool m_SelectionActivated = false;
		bool m_NeedsRefresh = true;

		bool m_IsRenaming = false;
		std::string m_RenamePath;
		char m_RenameBuffer[256]{};
		int m_RenameFrameCounter = 0;

		// Asset search filter (case-insensitive substring on file name). When
		// non-empty the grid shows m_SearchResults — matches from the whole
		// Assets/ tree — instead of the current folder. Rebuilt only on query
		// change / refresh, never per frame.
		char m_SearchBuffer[256]{};
		std::vector<DirectoryEntry> m_SearchResults;
		bool m_SearchTruncated = false;

		float m_TileSize = 80.0f;
		float m_TilePadding = 8.0f;

		bool m_ItemRightClicked = false;
		std::string m_PendingSceneLoad;
		std::string m_PendingPrefabEdit;
		std::vector<std::string> m_AssetClipboardPaths;
		bool m_AssetClipboardCut = false;

		// Deferred script creation - boilerplate/project script is written after rename is committed.
		enum class PendingScriptType { None, CSharp, CSharpEmpty, CSharpComponent, CSharpNativeComponent, CSharpSceneSystem, CSharpGlobalSystem, CSharpDataAsset, EntityPrefab };
		PendingScriptType m_PendingScriptType = PendingScriptType::None;
		std::string m_PendingScriptDir;  // parent directory for the new script
		EntityHandle m_PendingPrefabSourceEntity = entt::null;

		ThumbnailCache m_Thumbnails;

		std::unordered_map<std::string, std::vector<SpriteSlice>> m_SliceCache;
		std::unordered_set<std::string> m_ExpandedTextures;

		struct PendingCreationPrompt {
			std::filesystem::path PreferredPath;
			std::string DisplayName;
			std::function<void(const std::filesystem::path&)> DoCreate;
		};
		std::unique_ptr<PendingCreationPrompt> m_PendingCreationPrompt;
		// Set by CreateAssetWithCollisionCheck when it stores a pending
		// request; drained on the next RenderCreationCollisionPrompt frame
		// to call ImGui::OpenPopup exactly once.
		bool m_OpenCreationPromptThisFrame = false;

		// Pending asset-deletion confirmation. Paths are captured when the
		// user requests a delete; the prompt opens once and deletes them on
		// confirm. Drained by RenderDeleteConfirmationPrompt.
		std::vector<std::string> m_PendingDeletePaths;
		bool m_OpenDeletePromptThisFrame = false;

		// Paths whose thumbnails need invalidating. Filled by DeleteEntry,
		// drained at the top of the NEXT frame's Render(). Destroying a
		// Texture2D synchronously inside Render() while ImGui has already
		// recorded an Image draw command for that texture leaves the wgpu
		// backend with a dangling WGPUTextureView pointer at the end-of-
		// frame dispatch — the imgui_impl_wgpu draw walks pcmd->TexID, casts
		// it back to WGPUTextureView, and calls wgpuDeviceCreateBindGroup;
		// freed pointer = native crash. Deferring one frame lets the prior
		// frame's draw dispatch complete first.
		std::vector<std::string> m_PendingThumbnailInvalidates;
	};

}
