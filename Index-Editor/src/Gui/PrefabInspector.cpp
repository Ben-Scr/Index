#include <pch.hpp>
#include "Gui/PrefabInspector.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Core/Log.hpp"
#include "Editor/EditorComponentRegistration.hpp"
#include "Inspector/ReferencePicker.hpp"
#include "Gui/AddComponentPopup.hpp"
#include "Gui/Icons.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptDiscovery.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/SceneSerializerShared.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
#include <span>
#include <vector>

namespace Index {

	PrefabInspector::PrefabInspector() = default;
	PrefabInspector::~PrefabInspector() = default;

	void PrefabInspector::Open(const std::string& prefabPath) {
		Close();

		m_PrefabScene = Scene::CreateDetachedScene("##PrefabInspector");
		m_PrefabPath = prefabPath;

		if (!File::Exists(prefabPath)) {
			IDX_CORE_WARN_TAG("PrefabInspector", "Prefab file not found: {}", prefabPath);
			return;
		}

		Json::Value root;
		std::string readError;
		if (!SceneSerializerStorage::ReadRootFromFile(prefabPath, root, &readError) || !root.IsObject()) {
			IDX_CORE_ERROR_TAG("PrefabInspector", "Failed to parse {}: {}", prefabPath, readError);
			return;
		}

		// The .prefab envelope keeps both "Entity" (preferred) and a legacy
		// "prefab" key with the same payload. Prefer the new key, fall back.
		const Json::Value* entityValue = root.FindMember("Entity");
		if (!entityValue) entityValue = root.FindMember("prefab");
		if (!entityValue || !entityValue->IsObject()) {
			IDX_CORE_WARN_TAG("PrefabInspector", "No Entity/prefab block in {}", prefabPath);
			return;
		}

		// Deserialize as a Scene-origin (not Prefab-instance) entity so the inspector round-trips via SaveEntityToFile without leaking instance metadata.
		m_RootEntity = SceneSerializer::DeserializeEntityFromValue(*m_PrefabScene, root);
		// DeserializeEntityFromValue marks the scene dirty as a side effect; reset so only real user edits flag dirty.
		m_PrefabScene->ClearDirty();
	}

	void PrefabInspector::Close() {
		// Destroy hooks gated by Scene::IsDetached() skip, so physics/audio/script subsystems are not affected.
		m_RootEntity = entt::null;
		m_PrefabScene.reset();
		m_PrefabPath.clear();
	}

	bool PrefabInspector::HasUnsavedChanges() const {
		return m_PrefabScene && m_PrefabScene->IsDirty();
	}

	bool PrefabInspector::Render() {
		if (!m_PrefabScene || m_RootEntity == entt::null || !m_PrefabScene->IsValid(m_RootEntity)) {
			ImGui::TextDisabled("No prefab loaded.");
			return false;
		}

		const auto& registry = SceneManager::Get().GetComponentRegistry();
		Entity rootEntity = m_PrefabScene->GetEntity(m_RootEntity);
		const std::span<const Entity> entitySpan(&rootEntity, 1);

		const std::string filename = std::filesystem::path(m_PrefabPath).filename().string();
		ImGui::TextDisabled("Prefab:");
		ImGui::SameLine();
		ImGui::TextUnformatted(filename.c_str());
		ImGui::SameLine();
		const bool dirty = m_PrefabScene->IsDirty();
		ImGui::TextDisabled(dirty ? "(saving)" : "(auto-saved)");
		ImGui::Separator();

		// Component dispatch — same pattern as the entity inspector but without
		// clipboard / copy / paste / reset (those can come in v2).
		std::type_index pendingRemoval = typeid(void);

		registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
			if (info.category != ComponentCategory::Component) return;
			if (info.displayName == "Name") return; // Edited via the entity name field, if shown.
			if (!info.has || !info.has(rootEntity)) return;

			if (info.displayName == "Scripts") {
				DispatchComponentInspector(info, entitySpan);
				return;
			}

			// Mirrors the entity inspector guard (ImGuiEditorLayer): dynamic
			// `:IComponent` components render through the paired Scripts entry,
			// so the registry wrapper here would just be an empty section.
			if (info.isDynamic && !info.drawInspector && info.properties.empty()) {
				return;
			}

			bool removeRequested = false;
			bool open = ImGuiUtils::BeginComponentSection(info.displayName.c_str(), removeRequested,
				[]() {});
			if (removeRequested) {
				pendingRemoval = typeId;
			}

			if (open) {
				DispatchComponentInspector(info, entitySpan);
				ImGuiUtils::EndComponentSection();
			}
		});

		if (pendingRemoval != typeid(void)) {
			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (typeId == pendingRemoval && info.remove) {
					info.remove(rootEntity);
				}
			});
			m_PrefabScene->MarkDirty();
		}

		// Render the unified reference-picker popup once per inspector frame.
		ReferencePicker::RenderPopup();

		ImGui::Separator();
		const float buttonWidth = ImGui::GetContentRegionAvail().x;
		if (Icons::ButtonWithIcon(Icons::Type::Plus, "Add Component", ImVec2(buttonWidth, 0), true)) {
			ImGui::OpenPopup("AddPrefabComponentPopup");
			m_AddComponentSearchBuffer[0] = '\0';
		}

		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
				// Use 1-arg string constructor: the (data, DataSize) form embeds the trailing '\0' and breaks IsCSharpScriptExtension(".cs\0").
				std::string droppedPath(static_cast<const char*>(payload->Data));
				std::vector<EditorScriptDiscovery::ScriptEntry> droppedScripts;
				EditorScriptDiscovery::CollectScriptFile(std::filesystem::path(droppedPath), droppedScripts);
				for (const auto& scriptEntry : droppedScripts) {
					if (scriptEntry.IsSceneSystem || scriptEntry.IsGlobalSystem) {
						continue;
					}
					if (scriptEntry.ClassName.empty() || scriptEntry.Type == ScriptType::Unknown) {
						continue;
					}
					if (!rootEntity.HasComponent<ScriptComponent>()) {
						rootEntity.AddComponent<ScriptComponent>();
					}
					auto& scriptComponent = rootEntity.GetComponent<ScriptComponent>();
					if (scriptEntry.IsManagedComponent) {
						if (!scriptComponent.HasManagedComponent(scriptEntry.ClassName)) {
							scriptComponent.AddManagedComponent(scriptEntry.ClassName);
							m_PrefabScene->MarkDirty();
						}
					}
					else if (!scriptComponent.HasScript(scriptEntry.ClassName, scriptEntry.Type)) {
						scriptComponent.AddScript(scriptEntry.ClassName, scriptEntry.Type);
						m_PrefabScene->MarkDirty();
					}
				}
			}
			ImGui::EndDragDropTarget();
		}

		RenderAddComponentPopup("AddPrefabComponentPopup", *m_PrefabScene, entitySpan,
			m_AddComponentSearchBuffer, sizeof(m_AddComponentSearchBuffer));

		// Only dirty on actual value edits (ActiveIdHasBeenEditedThisFrame); plain focus/click does not set this flag, so tabbing through fields without changes is clean.
		const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		const ImGuiContext& g = *ImGui::GetCurrentContext();
		if (windowFocused && g.ActiveId != 0 && g.ActiveIdHasBeenEditedThisFrame) {
			m_PrefabScene->MarkDirty();
		}

		const bool itemActive = ImGui::IsAnyItemActive() && windowFocused;
		const bool dirtyForSave = m_PrefabScene->IsDirty();
		if (dirtyForSave && !itemActive) {
			Save();
		}

		return dirtyForSave;
	}

	bool PrefabInspector::Save() {
		if (!m_PrefabScene || m_RootEntity == entt::null) return false;

		// Capture old source JSON before overwriting: propagation diffs against the old source to preserve per-instance overrides.
		Json::Value previousSourceRoot;
		bool havePreviousSource = false;
		if (File::Exists(m_PrefabPath)) {
			Json::Value previousRoot;
			std::string readError;
			if (SceneSerializerStorage::ReadRootFromFile(m_PrefabPath, previousRoot, &readError) && previousRoot.IsObject()) {
				previousSourceRoot = previousRoot;
				havePreviousSource = true;
			}
			else {
				// File unreadable (locked/corrupt): bail and leave dirty for retry; overwriting without the old source would orphan live instance overrides.
				IDX_CORE_WARN_TAG("PrefabInspector",
					"Pre-save read of '{}' failed ({}); leaving prefab dirty for retry.",
					m_PrefabPath, readError);
				return false;
			}
		}

		if (!SceneSerializer::SaveEntityToFile(*m_PrefabScene, m_RootEntity, m_PrefabPath)) {
			return false;
		}

		// MUST propagate before ClearDirty: a mid-loop failure leaves dirty set for retry; ClearDirty first would leave instances stale with no retry hook.
		const uint64_t prefabGuid = AssetRegistry::GetOrCreateAssetUUID(m_PrefabPath);
		if (prefabGuid != 0 && havePreviousSource) {
			PropagateToLiveInstances(prefabGuid, previousSourceRoot);
		}

		m_PrefabScene->ClearDirty();
		return true;
	}

	void PrefabInspector::PropagateToLiveInstances(uint64_t prefabGuid,
		const Json::Value& previousSourceEntity) {
		SceneManager::Get().ForeachLoadedScene([&](Scene& scene) {
			if (&scene == m_PrefabScene.get()) return;

			std::vector<EntityHandle> instancesToRefresh;
			auto view = scene.GetRegistry().view<EntityMetaDataComponent>();
			for (entt::entity ent : view) {
				const auto& meta = view.get<EntityMetaDataComponent>(ent).MetaData;
				if (meta.Origin != EntityOrigin::Prefab) continue;
				if (static_cast<uint64_t>(meta.PrefabGUID) != prefabGuid) continue;
				instancesToRefresh.push_back(ent);
			}

			bool anyRefreshed = false;
			for (EntityHandle instance : instancesToRefresh) {
				if (!scene.IsValid(instance)) continue;
				EntityHandle replacement = SceneSerializer::RefreshPrefabInstance(scene, instance, previousSourceEntity);
				// Returns original handle when the source is orphaned (no-op); only a different handle means a real swap occurred.
				if (replacement != entt::null && replacement != instance) {
					anyRefreshed = true;
				}
			}

			if (anyRefreshed) {
				scene.MarkDirty();
			}
		});
	}

}
