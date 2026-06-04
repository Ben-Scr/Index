#include <pch.hpp>
#include "Systems/ImGuiEditorLayer.hpp"

#include <imgui.h>

#include <cstdio>

#include "Components/Forward.hpp"
#include "Components/General/General.hpp"
#include "Components/UI/UI.hpp"
#include "Components/Tags.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Core/Platform/Win32BuildProgressWindow.hpp"
#include "Graphics/Framebuffer.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Graphics/GizmoRenderer.hpp"
#include "Gui/ImGuiImplWebGPU.hpp"
#include "Profiling/Profiler.hpp"
#include "Graphics/Gizmo.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/ComponentRegistry.hpp"
#include <Scene/EntityHelper.hpp>

#include "Graphics/TextureManager.hpp"
#include "Gui/CustomTitlebar.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Core/Version.hpp"
#include "Serialization/Path.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/File.hpp"
#include "Utils/Process.hpp"
#include "Packages/NuGetSource.hpp"
#include "Packages/GitHubSource.hpp"
#include "Editor/EditorPreferences.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Gui/EditorIcons.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scripting/ScriptSystem.hpp"
#include "Systems/TransformHierarchySystem.hpp"
#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace Index {
	void ImGuiEditorLayer::EnsureViewportFramebuffer(int width, int height) {
		m_EditorViewFBO.Recreate(width, height);
	}

	void ImGuiEditorLayer::DestroyViewportFramebuffer() {
		m_EditorViewFBO.Destroy();
	}

	void ImGuiEditorLayer::OnPreRender(Application& app) {
		INDEX_PROFILE_SCOPE("Editor PreRender");
		(void)app;

		EditorPreferences::ApplySystemThemeIfNeeded();

		// Startup splash: holds (static) while the deferred startup load runs
		// behind it — RequestStartupLoad makes the scene load happen now (not
		// after detach), and it fades once IsStartupLoadComplete. While opaque we
		// return early (the editor UI also can't build yet — no active scene, the
		// check below would bail anyway).
		if (m_Splash.IsActive()) {
			Application::RequestStartupLoad();
			m_Splash.Render(app, Application::IsStartupLoadComplete());
			if (m_Splash.IsCovering()) {
				return;
			}
		}

		// MUST drain before the active-scene early-return: prefab-edit is valid with no active scene; gating it on the scene check silently no-ops double-click/right-click→Open in that case.
		if (!Application::GetIsPlaying() && m_AssetBrowserInitialized && m_BuildState == 0) {
			std::string earlyPendingPrefabEdit = m_AssetBrowser.TakePendingPrefabEdit();
			if (!earlyPendingPrefabEdit.empty()) {
				IDX_INFO_TAG("PrefabEdit", "Opening prefab from asset browser: {}", earlyPendingPrefabEdit);
				OpenPrefabForEditing(earlyPendingPrefabEdit);
			}
		}

		Scene* activeScene = SceneManager::Get().GetActiveScene();
		if (!activeScene) {
			return;
		}
		Scene& scene = *activeScene;

		// 1-frame delay (1→2→3): lets the progress overlay paint before the worker launches; without it the overlay wouldn't appear before dotnet build's first stage.
		if (m_BuildState == 1) {
			m_BuildState = 2;
		} else if (m_BuildState == 2) {
			ExecuteBuildAsync();
			m_BuildState = 3;
		} else if (m_BuildState == 3 && m_BuildFuture.valid()
			&& m_BuildFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			// Worker finished. Drain it (rethrows any exception that
			// escaped onto the worker; we log + recover instead of
			// taking down the editor).
			try {
				m_BuildFuture.get();
			} catch (const std::exception& e) {
				IDX_ERROR_TAG("Build", "Build worker crashed: {}", e.what());
				m_BuildSucceeded.store(false, std::memory_order_relaxed);
			} catch (...) {
				IDX_ERROR_TAG("Build", "Build worker crashed: unknown exception");
				m_BuildSucceeded.store(false, std::memory_order_relaxed);
			}
			m_BuildState = 0;

			// ShellExecuteA must run on the UI thread: it can pop modal dialogs on its calling thread.
#ifdef IDX_PLATFORM_WINDOWS
			if (m_BuildSucceeded.load(std::memory_order_relaxed) && !m_BuildOutputDir.empty()) {
				ShellExecuteA(nullptr, "open", m_BuildOutputDir.c_str(),
					nullptr, nullptr, SW_SHOWNORMAL);
			}
#endif

			// Launch the built game if "Build and Play" was clicked
			if (m_BuildAndPlay && m_BuildSucceeded.load(std::memory_order_relaxed)) {
				m_BuildAndPlay = false;
				IndexProject* project = ProjectManager::GetCurrentProject();
				if (project) {
					std::string outputDir = m_BuildOutputDirBuffer;
					if (outputDir.empty())
						outputDir = Path::Combine(project->RootDirectory, "Builds");
#ifdef IDX_PLATFORM_WINDOWS
					// ExecutableName overrides the exe stem; ExecuteBuild uses the same rule, so read both off the project to stay in lockstep.
					const std::string exeStem = project->ExecutableName.empty()
						? project->Name : project->ExecutableName;
					auto exePath = std::filesystem::path(outputDir) / (exeStem + ".exe");
					if (std::filesystem::exists(exePath)) {
						std::string cmd = "\"" + std::filesystem::canonical(exePath).string() + "\"";
						// Proper UTF-8 → wide conversion; byte-wise iterator construction corrupts non-ASCII paths (Unicode folders, accented names).
						std::wstring wcmd;
						{
							const int needed = MultiByteToWideChar(CP_UTF8, 0,
								cmd.c_str(), static_cast<int>(cmd.size()), nullptr, 0);
							if (needed > 0) {
								wcmd.resize(static_cast<size_t>(needed));
								MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(),
									static_cast<int>(cmd.size()), wcmd.data(), needed);
							}
						}
						std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
						buf.push_back(L'\0');
						STARTUPINFOW si{}; si.cb = sizeof(si);
						PROCESS_INFORMATION pi{};
						if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr,
							FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
							const DWORD errorCode = GetLastError();
							IDX_ERROR_TAG("Editor",
								"Failed to launch built game '{}': error {}",
								exePath.string(), errorCode);
						}
						else {
							CloseHandle(pi.hProcess);
							CloseHandle(pi.hThread);
							IDX_INFO_TAG("Build", "Launched: {}", exePath.string());
						}
					} else {
						IDX_CORE_ERROR_TAG("Build", "Built executable not found: {}", exePath.string());
					}
#endif
				}
			}
			// Reset m_BuildAndPlay even on failure so a subsequent
			// "Build" click doesn't unexpectedly auto-launch a stale
			// state from the failed run.
			m_BuildAndPlay = false;
		}

		// Process OS file drops — forward to AssetBrowser
		{
			auto* app = Application::GetInstance();
			if (app) {
				auto drops = app->TakePendingFileDrops();
				if (!drops.empty() && m_AssetBrowserInitialized && m_BuildState == 0) {
					m_AssetBrowser.OnExternalFileDrop(drops);
				}
			}
		}

		// Process deferred scene drop from Hierarchy drag-and-drop (blocked during Play Mode and active build)
		if (!m_PendingSceneFileDrop.empty() && !Application::GetIsPlaying() && m_BuildState == 0) {
			std::string dropPath = m_PendingSceneFileDrop;
			m_PendingSceneFileDrop.clear();

			std::string sceneName = std::filesystem::path(dropPath).stem().string();
			auto& sm = SceneManager::Get();
			if (!sm.HasSceneDefinition(sceneName))
				sm.RegisterScene(sceneName);
			if (!sm.IsSceneLoaded(sceneName)) {
				auto weakScene = sm.LoadSceneAdditive(sceneName);
				if (auto loaded = weakScene.lock()) {
					SceneSerializer::LoadFromFile(*loaded, dropPath);
					m_EntityOrder.clear(); m_EntityOrderDirty = true;
				}
			}
		}

		// Process deferred scene switch (blocked during Play Mode and active build)
		if (!m_PendingSceneSwitch.empty() && !Application::GetIsPlaying() && m_BuildState == 0) {
			std::string switchPath = m_PendingSceneSwitch;
			m_PendingSceneSwitch.clear();

			auto& sm = SceneManager::Get();
			Scene* active = sm.GetActiveScene();
			if (active) {
				// Exclusive switch: unload every scene except the active one (iterated by snapshot so UnloadScene doesn't invalidate the loop).
				const std::string activeName = active->GetName();
				for (const std::string& name : sm.GetLoadedSceneNames()) {
					if (name != activeName) sm.UnloadScene(name);
				}

				SceneSerializer::LoadFromFile(*active, switchPath);
				m_EntityOrder.clear(); m_EntityOrderDirty = true;
				IndexProject* project = ProjectManager::GetCurrentProject();
				if (project) {
					project->LastOpenedScene = std::filesystem::path(switchPath).stem().string();
					project->Save();
				}
			}
		}

		// Intercept Asset Browser scene load. Active build still blocks; play
		// mode no longer does — double-clicking a scene during play exits play
		// mode and switches to the chosen scene (the open gesture IS the
		// intent to leave). Save prompt is skipped in that case because
		// play-mode mutations are intentionally discarded by the restore.
		if (m_BuildState == 0) {
			std::string pendingLoad = m_AssetBrowser.TakePendingSceneLoad();
			if (!pendingLoad.empty()) {
				if (Application::GetIsPlaying()) {
					RestoreEditorSceneAfterPlaymode();
					m_PendingSceneSwitch = pendingLoad;
				}
				else {
					Scene* active = SceneManager::Get().GetActiveScene();
					if (active && active->IsDirty()) {
						m_ConfirmDialogPendingPath = pendingLoad;
						m_ShowSaveConfirmDialog = true;
					} else {
						m_PendingSceneSwitch = pendingLoad;
					}
				}
			}
		}

		// (Prefab-edit drain is hoisted above the active-scene early return
		// at the top of OnPreRender — see comment there.)

		// Ctrl+S priority: prefab-edit mode > prefab inspector > all dirty scenes.
		// Prefab saves allowed during play (detached scene is independent); scene save blocked to avoid persisting play-mode state.
		if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && m_BuildState == 0) {
			if (IsInPrefabEditMode()) {
				// Full prefab-edit mode: save the detached scene back to its
				// .prefab without exiting edit mode. Without this branch,
				// Ctrl+S would save the (hidden) active scene instead of the
				// thing the user is actually looking at.
				SavePrefabEditChanges();
			}
			else if (m_PrefabInspector.IsOpen() && m_PrefabInspector.HasUnsavedChanges()) {
				m_PrefabInspector.Save();
			}
			else if (!Application::GetIsPlaying()) {
				IndexProject* project = ProjectManager::GetCurrentProject();
				if (project) {
					bool savedAny = false;
					for (auto& weakScene : SceneManager::Get().GetLoadedScenes()) {
						auto scene = weakScene.lock();
						if (!scene || !scene->IsDirty()) continue;
						std::string scenePath = project->GetSceneFilePath(scene->GetName());
						if (SceneSerializer::SaveToFile(*scene, scenePath)) {
							savedAny = true;
						}
					}
					if (savedAny) {
						Scene* active = SceneManager::Get().GetActiveScene();
						if (active) {
							project->LastOpenedScene = active->GetName();
						}
						project->Save();
					}
				}
			}
		}

		// Intercept quit request: exit playmode first, then check for unsaved changes
		if (Application::IsQuitRequested()) {
			// Refuse to quit mid-build — would kill the worker thread and leave partial artifacts on disk.
			if (m_BuildState != 0) {
				Application::CancelQuit();
			}
			else {
				// Exit playmode and restore scene first
				if (Application::GetIsPlaying()) {
					RestoreEditorSceneAfterPlaymode();
				}

				// Auto-save dirty prefab edits on quit — adding another modal on top of the scene quit dialog is too noisy.
				if (m_PrefabEditScene && m_PrefabEditScene->IsDirty()) {
					SavePrefabEditChanges();
				}
				// Always close prefab edit so destructors run before SceneManager cleanup.
				if (m_PrefabEditScene) {
					ClosePrefabEditing(false);
				}

				Scene* active = SceneManager::Get().GetActiveScene();
				if (active && active->IsDirty()) {
					m_ShowQuitSaveDialog = true;
					Application::CancelQuit();
				}
			}
		}

		// MUST precede RenderDockspaceRoot: titlebar publishes its height so the dockspace offsets correctly.
		{
			std::string titlebarText = "Index Editor " + std::string(IDX_VERSION);
			if (IndexProject* project = ProjectManager::GetCurrentProject()) {
				titlebarText += " - " + project->Name;
			}
			EditorRuntime::RenderTitlebar(EditorRuntime::TitlebarConfig{ std::move(titlebarText), /*Centered=*/true });
		}
		RenderDockspaceRoot();

		// Block menu bar during build (worker is reading shared state); modals stay outside this gate so they remain dismissable.
		ImGui::BeginDisabled(m_BuildState > 0);
		RenderMainMenu(scene);
		ImGui::EndDisabled();

		// Save confirmation modal dialog (scene switch)
		if (m_ShowSaveConfirmDialog) {
			ImGui::OpenPopup("Save Changes?");
			m_ShowSaveConfirmDialog = false;
		}
		ImGuiUtils::CenterNextModal();
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::BeginPopupModal("Save Changes?", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			Scene* active = SceneManager::Get().GetActiveScene();
			std::string activeName = active ? active->GetName() : "Scene";
			ImGui::Text("Save changes to %s before opening?", activeName.c_str());
			ImGui::Spacing();

			if (ImGui::Button("Save", ImVec2(100, 0))) {
				if (active) {
					IndexProject* project = ProjectManager::GetCurrentProject();
					if (project) {
						std::string savePath = project->GetSceneFilePath(active->GetName());
						SceneSerializer::SaveToFile(*active, savePath);
					}
				}
				m_PendingSceneSwitch = m_ConfirmDialogPendingPath;
				m_ConfirmDialogPendingPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Don't Save", ImVec2(100, 0))) {
				m_PendingSceneSwitch = m_ConfirmDialogPendingPath;
				m_ConfirmDialogPendingPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				m_ConfirmDialogPendingPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// Prefab-edit discard modal (back button with unsaved changes) — distinct from the prefab-inspector save prompt below (fired on asset selection change).
		if (m_ShowPrefabEditDiscardPrompt) {
			ImGui::OpenPopup("Unsaved Prefab Changes");
			m_ShowPrefabEditDiscardPrompt = false;
		}
		ImGuiUtils::CenterNextModal();
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::BeginPopupModal("Unsaved Prefab Changes", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			// Stem (no .prefab extension) keeps the message consistent
			// with the centered title in the prefab toolbar — both refer
			// to the same prefab by the same name.
			std::string editPrefabName = std::filesystem::path(m_PrefabEditPath).stem().string();
			if (editPrefabName.empty()) editPrefabName = "the prefab";
			ImGui::Text("Save unsaved changes to %s?", editPrefabName.c_str());
			ImGui::Spacing();

			if (ImGui::Button("Save", ImVec2(100, 0))) {
				ClosePrefabEditing(true);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Don't Save", ImVec2(100, 0))) {
				ClosePrefabEditing(false);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				// Stay in edit mode; modal closes without altering state.
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// Prefab save/discard modal — fired when the user picks a different
		// asset while a dirty prefab is open in the inspector.
		if (m_ShowPrefabSavePrompt) {
			ImGui::OpenPopup("Save Prefab Changes?");
			m_ShowPrefabSavePrompt = false;
		}
		ImGuiUtils::CenterNextModal();
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::BeginPopupModal("Save Prefab Changes?", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			std::string prefabName = std::filesystem::path(m_PrefabInspector.GetCurrentPath()).filename().string();
			if (prefabName.empty()) prefabName = "the prefab";
			ImGui::Text("Save changes to %s before switching?", prefabName.c_str());
			ImGui::Spacing();

			if (ImGui::Button("Save", ImVec2(100, 0))) {
				m_PrefabInspector.Save();
				if (!m_PendingPrefabSwitchPath.empty()) {
					m_PrefabInspector.Open(m_PendingPrefabSwitchPath);
					m_PrefabInspectorPath = m_PendingPrefabSwitchPath;
					m_PendingPrefabSwitchPath.clear();
				}
				else {
					m_PrefabInspector.Close();
					m_PrefabInspectorPath.clear();
				}
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Don't Save", ImVec2(100, 0))) {
				if (!m_PendingPrefabSwitchPath.empty()) {
					m_PrefabInspector.Open(m_PendingPrefabSwitchPath);
					m_PrefabInspectorPath = m_PendingPrefabSwitchPath;
					m_PendingPrefabSwitchPath.clear();
				}
				else {
					m_PrefabInspector.Close();
					m_PrefabInspectorPath.clear();
				}
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				m_PendingPrefabSwitchPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// Quit confirmation modal dialog
		if (m_ShowQuitSaveDialog) {
			ImGui::OpenPopup("Save Before Quit?");
			m_ShowQuitSaveDialog = false;
		}
		ImGuiUtils::CenterNextModal();
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::BeginPopupModal("Save Before Quit?", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			Scene* active = SceneManager::Get().GetActiveScene();
			std::string activeName = active ? active->GetName() : "Scene";
			ImGui::Text("Save changes to %s before closing?", activeName.c_str());
			ImGui::Spacing();

			if (ImGui::Button("Save", ImVec2(100, 0))) {
				if (active) {
					IndexProject* project = ProjectManager::GetCurrentProject();
					if (project) {
						std::string savePath = project->GetSceneFilePath(active->GetName());
						SceneSerializer::SaveToFile(*active, savePath);
						project->LastOpenedScene = active->GetName();
						project->Save();
					}
				}
				ImGui::CloseCurrentPopup();
				Application::ConfirmQuit();
			}
			ImGui::SameLine();
			if (ImGui::Button("Don't Save", ImVec2(100, 0))) {
				ImGui::CloseCurrentPopup();
				Application::ConfirmQuit();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// Suppress script recompilation while a script is being created/renamed
		if (scene.HasSystem<ScriptSystem>()) {
			auto* scriptSys = scene.GetSystem<ScriptSystem>();
				if (scriptSys)
					scriptSys->SetRecompileSuppressed(m_AssetBrowser.IsCreatingScript());
		}

		if (Application::GetIsPlaying()
			&& EditorPreferences::GetExitPlayModeOnRecompilation()
			&& scene.HasSystem<ScriptSystem>())
		{
			if (auto* scriptSys = scene.GetSystem<ScriptSystem>();
				scriptSys && scriptSys->IsRebuilding())
			{
				RestoreEditorSceneAfterPlaymode();
			}
		}

		PollPendingPlayModeRequest(scene);

		// Reset every frame so a hidden/closed Game View falls back to the OS viewport.
		Window::ClearUIRegion();

		ImGui::BeginDisabled(m_BuildState > 0);

		RenderToolbar();
		RenderEntitiesPanel();
		// Inspector/editor view target the prefab-edit scene when active; Game View always uses the real active scene.
		Scene* contextScene = GetContextScene();
		RenderInspectorPanel(contextScene ? *contextScene : scene);
		// Also propagate the detached prefab scene — ForeachLoadedScene skips it since it isn't in SceneManager.
		SceneManager::Get().ForeachLoadedScene([](Scene& s) {
			TransformHierarchySystem::Propagate(s);
			});
		if (IsInPrefabEditMode() && m_PrefabEditScene) {
			TransformHierarchySystem::Propagate(*m_PrefabEditScene);
		}
		// Tick before both viewports so the simulation advances even when the user is on the Game View tab.
		TickParticlePreview(contextScene ? *contextScene : scene);
		RenderEditorView(contextScene ? *contextScene : scene);
		RenderGameView(scene);
		RenderProjectPanel();
		RenderLogPanel();
		RenderBuildPanel();
		RenderBuildProfilesPanel();
		RenderProjectSettingsPanel();
		RenderPackageManagerPanel();
		m_EditorPreferencesPanel.Render(&m_ShowEditorPreferences);

		ImGui::EndDisabled();

		TickSplashPreview();

		// Profiler panel — Ctrl+F6 toggle. Panel manages its own visibility-gating internally.
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F6)) {
			m_ShowProfiler = !m_ShowProfiler;
		}
		if (m_ShowProfiler) {
			m_ProfilerPanel.Render(&m_ShowProfiler);
		}

		// Drain sprite editor path at render time — opening during inspector build trips ImGui's Begin/End guard.
		if (!m_PendingSpriteEditorPath.empty()) {
			m_SpriteEditorPanel.OpenTexture(m_PendingSpriteEditorPath, m_PendingSpriteEditorAssetId);
			m_PendingSpriteEditorPath.clear();
			m_PendingSpriteEditorAssetId = 0;
		}
		if (m_ShowSpriteEditor) {
			m_SpriteEditorPanel.Render(&m_ShowSpriteEditor);
		}

		// Loading popup priority: build > script recompile > package install (build already includes a script compile sub-stage).
		bool showPopup = false;
		std::string popupTitle;
		std::string popupStage;
		float popupProgress = 0.0f;

		if (m_BuildState > 0) {
			showPopup = true;
			popupTitle = "Building Project...";
			// Snapshot both under the same lock so the stage label always
			// matches the bar fraction in any given frame.
			std::lock_guard<std::mutex> lk(m_BuildProgressMutex);
			popupStage = m_BuildStage;
			popupProgress = m_BuildProgress;
		} else if (auto* scriptSys = scene.HasSystem<ScriptSystem>()
				? scene.GetSystem<ScriptSystem>() : nullptr;
			scriptSys && (scriptSys->IsScriptRebuildRunning() || scriptSys->IsNativeRebuildRunning())) {
			showPopup = true;
			popupTitle = scriptSys->IsNativeRebuildRunning()
				? "Compiling Native Scripts..."
				: "Compiling Scripts...";
			// No real progress signal from the script-rebuild path, so
			// loop the bar 0→1 the same way the old ImGui overlay did.
			const float elapsed = scriptSys->GetActiveRebuildElapsedSeconds();
			popupProgress = fmodf(elapsed * 0.4f, 1.0f);
		} else if (m_PackageManagerPanel.GetActiveLoadingPopup(popupTitle, popupStage, popupProgress)) {
			showPopup = true;
		}

		if (showPopup) {
			Win32BuildProgressWindow::Show(popupTitle); // idempotent — retitles if already shown
			Win32BuildProgressWindow::Update(popupProgress, popupStage);
		} else if (Win32BuildProgressWindow::IsVisible()) {
			Win32BuildProgressWindow::Hide();
		}

		ImGui::End(); // Close dockspace opened in RenderDockspaceRoot.
	}
}
