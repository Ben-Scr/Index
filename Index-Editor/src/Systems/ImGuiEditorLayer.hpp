#pragma once
#include "Core/Layer.hpp"
#include "Core/Export.hpp"
#include "Collections/Color.hpp"
#include "Diagnostics/LogOverlay.hpp"
#include "Diagnostics/StatsOverlay.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Collections/Ids.hpp"
#include "Collections/Viewport.hpp"
#include "Core/Log.hpp"
#include "Gui/AssetBrowser.hpp"
#include "Gui/BuildProfilesPanel.hpp"
#include "Gui/EditorPreferencesPanel.hpp"
#include "Gui/PackageManagerPanel.hpp"
#include "Gui/PrefabInspector.hpp"
#include "Gui/ProfilerPanel.hpp"
#include "Gui/SpriteEditorPanel.hpp"
#include "Gui/SplashScreen.hpp"
#include "Packages/PackageManager.hpp"
#include "Editor/EditorCamera.hpp"
#include "Graphics/Framebuffer.hpp"
#include "Graphics/Gizmo.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureHandle.hpp"
#include "Serialization/FileWatcher.hpp"


#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>
#include <chrono>

namespace Index {

	class IndexProject;

	enum class EditorViewDrawMode : uint8_t {
		Default = 0,
		Triangle,
		Mixed,
	};

	class ImGuiEditorLayer : public Layer {
	public:
		using Layer::Layer;

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnPreRender(Application& app) override;
		void OnUpdate(Application& app, float dt) override;
		void OnEvent(Application& app, IndexEvent& event) override;
	private:
		struct LogEntry {
			std::string Message;
			Log::Level Level;
			std::string SourceFilePath;
			std::string SourceLinkText;
			int SourceLine = 0;
			// Byte span within `Message` of the clickable "<path>:line N"
			// substring. The renderer styles those bytes as a link inline
			// in the message text; equal start/end means no inline link.
			size_t SourceLinkStart = 0;
			size_t SourceLinkEnd = 0;
		};

		struct LogDispatchState {
			std::mutex Mutex;
			std::vector<LogEntry> PendingEntries;
		};

		struct PreviewTextureEntry {
			std::string CanonicalPath;
			std::unique_ptr<Texture2D> Texture;
			std::uint64_t LastTouchTick = 0;
		};

		// Per-panel render targets are RAII-managed Framebuffers — see
		// `Graphics/Framebuffer.hpp`. Resize is `m_*FBO.Recreate(w, h)`,
		// destruction is automatic on layer teardown.
		void EnsureViewportFramebuffer(int width, int height);
		void DestroyViewportFramebuffer();

		void RenderDockspaceRoot();
		void RenderMainMenu(Scene& scene);
		void RenderToolbar();

		void LaunchStandalone();
		void RenderEntitiesPanel();
		void RenderInspectorPanel(Scene& scene);
		void RenderEditorView(Scene& scene);
		void RenderGameView(Scene& scene);
		void TickParticlePreview(Scene& scene);
		void RenderLogPanel();
		void RenderProjectPanel();
		void RenderBuildPanel();
		void RenderBuildProfilesPanel();
		void RenderProjectSettingsPanel();
		void RenderSettings_Display(IndexProject& project, bool& changed, const std::string& filterLower);
		void RenderSettings_Graphics(IndexProject& project, bool& changed, const std::string& filterLower);
		void RenderSettings_Branding(IndexProject& project, bool& changed, const std::string& filterLower);
		void RenderSettings_Build(IndexProject& project, bool& changed, const std::string& filterLower);
		void RenderSettings_Editor(IndexProject& project, bool& changed, const std::string& filterLower);
		void RenderSettings_Systems(IndexProject& project, bool& changed, bool& outGlobalSystemsChanged, const std::string& filterLower);
		void TickSplashPreview();
		void RenderSceneSystemsInspector(Scene& scene);
		void ExecuteBuild();
		void ExecuteBuildAsync();
		void ReportBuildProgress(float progress, std::string_view stage);
		void RenderPackageManagerPanel();
		void RenderAssetInspector();
		void BeginPlayModeRequest(Scene& scene);
		void CompletePlayModeEntry(Scene& scene);
		void PollPendingPlayModeRequest(Scene& scene);
		void RestoreEditorSceneAfterPlaymode();
		void SelectSceneNode();
		void SelectEntity(EntityHandle entity);
		void ClearEntitySelection();
		bool IsEntitySelected(EntityHandle entity) const;
		std::vector<EntityHandle> GetSelectedEntities(const Scene& scene) const;
		void SetSingleEntitySelection(EntityHandle entity, int index);
		void ToggleEntitySelection(EntityHandle entity, int index);
		void SelectEntityRange(int index);

		void RebuildSelectionSet();
		void RecomputeInspectorSelectionCache(Scene& scene);
		void DrainPendingLogEntries();
		void RunAutoSaveTick(Application& app, float dt);
		void RunPrefabAutoSaveTick();
		void AppendLogEntry(LogEntry entry);
		void ClearLogEntries();
		void ResetEditorFocusCycle();
		void UpdateEditorCameraFocus(float dt);
		bool TryBuildEntityAABB(Scene& scene, EntityHandle entity, bool includeChildren, AABB& outAABB);
		void FocusSelectedEntity(Scene& scene);
		void DuplicateSelectedEntity(Scene& scene);
		// User-facing delete entry point: opens the confirmation dialog when
		// enabled in preferences, otherwise deletes immediately.
		void RequestDeleteSelectedEntity(Scene& scene);
		void DeleteSelectedEntity(Scene& scene);
		void BeginRenameSelectedEntity(Scene& scene);
		void UnpackSelectedPrefabs(Scene& scene);
		void CopySelectedEntities(Scene& scene);
		void CutSelectedEntities(Scene& scene);
		void PasteEntities(Scene& scene);
		EntityHandle FinishCreatedEditorEntity(Scene& scene, Entity parent, Entity created);
		EntityHandle RenderCreateEntityMenu(Scene& scene, Entity parent);
		std::string MakeEditorUniqueEntityName(Scene& scene, std::string_view baseName, EntityHandle ignoreEntity = entt::null) const;
		void EnsureEditorUniqueEntityName(Scene& scene, EntityHandle entity);
		void EnsureEditorUniqueEntityNames(Scene& scene, const std::vector<EntityHandle>& roots);
		bool SetEntityParentPreservingWorld(Scene& scene, EntityHandle child, Entity parent);
		// Reorders `dragged` to be the previous/next sibling of `target`; reparents across branches; returns true if the scene was actually mutated.
		bool MoveSiblingNextTo(Scene& scene, EntityHandle dragged, EntityHandle target, bool insertAfter);

		std::vector<EntityHandle> ResolveDraggedHierarchyEntities(Scene& scene, EntityHandle primary) const;

		// Serializes roots + subtrees through the clipboard path and re-creates them in targetScene; used by cross-scene drag-drop.
		std::vector<EntityHandle> MigrateEntitiesToScene(Scene& sourceScene, Scene& targetScene,
			const std::vector<EntityHandle>& sourceRoots, Entity targetParent);

		// ── Prefab edit mode ──────────────────────────────────────────
		bool OpenPrefabForEditing(const std::string& path);
		void ClosePrefabEditing(bool save);
		bool SavePrefabEditChanges();
		bool IsInPrefabEditMode() const { return m_PrefabEditScene != nullptr; }
		Scene* GetContextScene() const;

		bool HasEntityShortcutFocus() const;
		void DrawEditorComponentGizmos(Scene& scene, bool componentGizmosEnabled);
		const Texture2D* GetPreviewTexture(const std::filesystem::path& path);
		void TrimPreviewTextureCache();
		void ClearPreviewTextureCache();

		void RenderSceneIntoFBO(Framebuffer& fbo, Scene& scene,
			const glm::mat4& vp, const AABB& viewportAABB,
			bool withGizmos, bool sharedGizmosOnly = false,
			const Color& clearColor = Color::Background(),
			bool onlyPassedScene = false,
			bool uiInWorldSpace = false,
			EditorViewDrawMode drawMode = EditorViewDrawMode::Default,
			GizmoLayerMask gizmoLayerMask = GizmoLayerMask::All);

		EntityHandle m_ParticlePreviewEntity = entt::null;
		std::uint64_t m_ParticlePreviewSceneId = 0;
		EntityHandle m_SelectedEntity = entt::null;
		EntityHandle m_PressedEntity = entt::null;
		std::vector<EntityHandle> m_SelectedEntities;
		std::unordered_set<EntityHandle> m_SelectedEntitySet;
		// Bumped every time the selection mutates. Inspector / future
		// consumers compare against their own cached snapshot to decide
		// whether their derived state is still valid.
		std::uint64_t m_SelectionVersion = 0;
		int m_LastEntitySelectionIndex = -1;
		bool m_IsSceneNodeSelected = false;

		// Lazily recomputed when m_SelectionVersion changes; avoids per-frame N×M loops at large selection counts.
		struct InspectorSelectionCache {
			std::uint64_t Version = UINT64_MAX;
			std::vector<EntityHandle> Handles;
			std::unordered_set<std::type_index> CommonComponentTypes;
			std::vector<std::string> PartialComponents;
			bool NameUniform = true;
			std::string FirstName;
			bool EnabledUniform = true;
			bool EnabledFirst = true;
			bool StaticUniform = true;
			bool StaticFirst = false;
		};
		InspectorSelectionCache m_InspectorCache;
		EventId m_LogSubscriptionId{};
		std::vector<LogEntry> m_LogEntries;
		std::shared_ptr<LogDispatchState> m_LogDispatchState;
		bool m_ShowLogInfo = true;
		bool m_ShowLogWarn = true;
		bool m_ShowLogError = true;
		std::vector<PreviewTextureEntry> m_PreviewTextureCache;
		std::unordered_map<std::string, size_t> m_PreviewTextureLookup;
		std::uint64_t m_PreviewTextureTick = 0;
		static constexpr size_t kMaxPreviewTextures = 16;

		// Entity ordering for hierarchy drag-reorder
		std::vector<entt::entity> m_EntityOrder;

		// Rebuilt only when structural changes flip m_EntityOrderDirty; per-frame UI interactions (select/expand) do not.
		bool m_EntityOrderDirty = true;
		std::vector<entt::entity> m_RenderedEntityOrder;
		std::vector<int> m_RenderedEntityDepths;
		std::vector<entt::entity> m_VisibleEntityOrder;
		std::vector<int> m_VisibleEntityDepths;
		// Detects scene swaps that don't flip m_EntityOrderDirty; stale handles from a prior scene cause ImGui to assert if not caught here.
		// Scene* (not sceneId): two loaded scenes can collide on sceneId — a duplicated .scene file keeps its serialized id, and an
		// additively loaded script-only scene starts at the UUID default. Scene* is always distinct among currently-loaded scenes.
		const class Scene* m_EntityOrderSceneId = nullptr;

		// ── Prefab edit mode ──────────────────────────────────────────
		std::unique_ptr<Scene> m_PrefabEditScene;
		std::string m_PrefabEditPath;
		EntityHandle m_PrefabEditRootEntity = entt::null;
		bool m_PrefabEditDirty = false;
		// Camera state saved at OpenPrefabForEditing and restored on close so the user returns to their prior viewport.
		Vec2 m_PrefabEditSavedCameraPosition{ 0.0f, 0.0f };
		float m_PrefabEditSavedCameraOrthoSize = 5.0f;
		float m_PrefabEditSavedCameraZoom = 1.0f;
		bool m_PrefabEditHasSavedCameraState = false;
		bool m_ShowPrefabEditDiscardPrompt = false;
		static constexpr float k_PrefabEditClearR = 0.13f;
		static constexpr float k_PrefabEditClearG = 0.21f;
		static constexpr float k_PrefabEditClearB = 0.32f;

		std::unordered_set<uint32_t> m_CollapsedHierarchyEntities;

		std::unordered_set<uint32_t> m_CutEntities;

		std::string m_LastInteractedSceneName;

		EntityHandle m_RenamingEntity = entt::null;
		char m_EntityRenameBuffer[256]{};
		int m_EntityRenameFrameCounter = 0;

		Framebuffer m_EditorViewFBO;
		EditorCamera m_EditorCamera;
		bool m_IsEditorViewHovered = false;
		bool m_IsEditorViewFocused = false;
		EditorViewDrawMode m_EditorViewDrawMode = EditorViewDrawMode::Default;
		bool m_ShowGizmos = true;
		bool m_ShowPostProcessing = true;
		bool m_EditorCameraFocusActive = false;
		Vec2 m_EditorCameraFocusTarget{ 0.0f, 0.0f };
		float m_EditorCameraFocusOrthoSize = 5.0f;
		Vec2 m_EditorCameraFocusStartPosition{ 0.0f, 0.0f };
		float m_EditorCameraFocusStartOrthoSize = 5.0f;
		float m_EditorCameraFocusElapsed = 0.0f;
		EntityHandle m_FocusLastEntity = entt::null;
		bool m_FocusNextPressTight = false;

		bool m_IsGameViewActive = false;
		bool m_IsEditorViewActive = false;
		bool m_IsEntitiesPanelFocused = false;
		bool m_IsInspectorPanelFocused = false;

		Framebuffer m_GameViewFBO;
		bool m_IsGameViewHovered = false;
		bool m_IsGameViewFocused = false;
		int m_GameViewAspectPresetIndex = 0;
		bool m_GameViewAspectLoaded = false;
		bool m_GameViewVsync = true;
		bool m_GameViewVsyncLoaded = false;
		bool m_GameViewHasRendered = false;
		int m_LastGameViewFbW = 0;
		int m_LastGameViewFbH = 0;
		std::chrono::steady_clock::time_point m_LastGameViewRenderTime{};

		bool m_ShowGameViewStats = false;
		bool m_ShowGameViewLogs  = false;
		Index::Diagnostics::StatsOverlay m_GameViewStatsOverlay;
		// unique_ptr: lazily constructed to avoid subscribing to Log::OnLog before Application::Initialize.
		std::unique_ptr<Index::Diagnostics::LogOverlay> m_GameViewLogOverlay;

		Viewport m_EditorViewport{ 1, 1 };
		bool m_IsViewportHovered = false;
		bool m_IsViewportFocused = false;
		bool m_IsPlaying = false;

		AssetBrowser m_AssetBrowser;
		bool m_AssetBrowserInitialized = false;
		FileWatcher m_AssetWatcher;
		std::string m_AssetWatcherRoot;

		PrefabInspector m_PrefabInspector;
		std::string m_PrefabInspectorPath;
		// Save/discard prompt state for switching away from a dirty prefab.
		bool m_ShowPrefabSavePrompt = false;
		std::string m_PendingPrefabSwitchPath;

		// Queued rather than called directly to keep the OpenTexture → unsaved-changes-prompt flow at ImGui-render time.
		bool m_ShowSpriteEditor = false;
		std::string m_PendingSpriteEditorPath;
		uint64_t m_PendingSpriteEditorAssetId = 0;
		SpriteEditorPanel m_SpriteEditorPanel;

		std::string m_PendingSceneFileDrop;
		std::string m_PendingSceneSwitch;
		std::string m_ConfirmDialogPendingPath;
		bool m_ShowSaveConfirmDialog = false;

		// Entity-deletion confirmation (see RequestDeleteSelectedEntity and the
		// modal in ImGuiEditorLayerChrome). Scene is re-resolved by id at
		// confirm time so an unloaded scene can't leave a dangling pointer.
		bool m_ShowEntityDeleteConfirm = false;
		uint64_t m_PendingEntityDeleteSceneId = 0;
		size_t m_PendingEntityDeleteCount = 0;
		std::string m_PendingEntityDeleteLabel;
		char m_ComponentSearchBuffer[128]{};
		char m_SystemSearchBuffer[128]{};
		char m_GlobalSystemSearchBuffer[128]{};
		char m_ProjectSettingsSearchBuffer[256]{};
		std::string m_SelectedAssetPath;
		std::string m_ComponentClipboardJson;
		std::string m_EntityClipboardJson;

		std::vector<std::pair<std::string, std::string>> m_PlayModeScenes;
		// Name of the active scene at play-mode entry. Restored after exit so
		// a script-side LoadScene during play doesn't leak its choice into edit mode.
		std::string m_PlayModeActiveScene;
		bool m_PlayModeRecompilePending = false;
		int m_StepFrames = 0;

		float m_AutoSaveAccumulator = 0.0f;
		std::string m_AutoSaveLastScenePath;

		bool m_ShowQuitSaveDialog = false;
		bool m_QuitSaveDialogOpen = false;
		bool m_ShowBuildPanel = false;
		bool m_ShowBuildProfilesPanel = false;
		BuildProfilesPanel m_BuildProfilesPanel;
		bool m_BuildProfilesPanelInitialized = false;
		bool m_ShowPackageManager = false;
		bool m_ShowProfiler = false;
		ProfilerPanel m_ProfilerPanel;

		std::vector<std::string> m_BuildSceneList;
		int m_DraggedSceneIndex = -1;
		bool m_ShowProjectSettings = false;
	public:
		enum class SettingsCategory : uint8_t {
			Display = 0,
			Graphics,
			Branding,
			Build,
			Editor,
			Systems,
		};
	private:
		SettingsCategory m_SelectedSettingsCategory = SettingsCategory::Display;
		bool m_ShowEditorPreferences = false;
		EditorPreferencesPanel m_EditorPreferencesPanel;

		// Startup splash: holds (static) while the deferred startup load runs
		// behind it, then fades out. See EditorRuntime::SplashScreen.
		EditorRuntime::SplashScreen m_Splash;
		bool m_PackageManagerInitialized = false;
		bool m_SplashPreviewRequest = false;
		bool m_SplashPreviewActive = false;
		float m_SplashPreviewElapsed = 0.0f;
		TextureHandle m_SplashPreviewLogo;
		TextureHandle m_SplashPreviewBackground;
		std::uint32_t m_SplashPreviewTexRefToken = 0;
		PackageManager m_PackageManager;
		PackageManagerPanel m_PackageManagerPanel;
		std::string m_BuildOutputDir;
		char m_BuildOutputDirBuffer[512]{};
		char m_CustomDefineEntryBuffer[128]{}; // Build panel custom-define text input
		int m_BuildState = 0; // 0=idle, 1=pending, 2=launch worker, 3=running
		bool m_BuildAndPlay = false;
		// Progress + stage written together under m_BuildProgressMutex so UI never observes a mismatched pair.
		std::future<void> m_BuildFuture;
		std::atomic<bool>  m_BuildSucceeded{ true };
		std::mutex m_BuildProgressMutex;
		float m_BuildProgress{ 0.0f };  // protected by m_BuildProgressMutex
		std::string m_BuildStage;       // protected by m_BuildProgressMutex
		std::vector<entt::entity> m_EditorPausedAudioEntities; // AudioSources paused by editor, not by gameplay
		std::chrono::steady_clock::time_point m_BuildStartTime;
	};
}
