#include <pch.hpp>
#include "Gui/AddComponentPopup.hpp"

#include "Gui/Icons.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptDiscovery.hpp"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Index {

	namespace {

		std::string BuildScriptMenuLabel(const EditorScriptDiscovery::ScriptEntry& scriptEntry)
		{
			return scriptEntry.ClassName + "  " + scriptEntry.Extension;
		}

		bool AttachScriptToEntity(Entity entity, Scene& scene, const EditorScriptDiscovery::ScriptEntry& scriptEntry)
		{
			if (scriptEntry.ClassName.empty() || scriptEntry.Type == ScriptType::Unknown) {
				return false;
			}

			// Guard against hot-reload / scene-swap invalidating the captured Entity/Scene between frames.
			if (!entity.IsValid() || !scene.IsValid(entity.GetHandle())) {
				return false;
			}

			if (!entity.HasComponent<ScriptComponent>()) {
				entity.AddComponent<ScriptComponent>();
			}

			auto& scriptComponent = entity.GetComponent<ScriptComponent>();
			const bool alreadyHadScript = scriptComponent.HasScript(scriptEntry.ClassName, scriptEntry.Type);

			if (!alreadyHadScript) {
				scriptComponent.AddScript(scriptEntry.ClassName, scriptEntry.Type);
			}

			// MUST also seed DynamicComponentStorage; without it inspector renders fine but
			// Entity.HasNativeComponent<T>()/GetRef<T>() sees nothing in the ECS pool.
			bool storageAdded = false;
			if (scriptEntry.IsNativeComponent) {
				auto& componentRegistry = SceneManager::Get().GetComponentRegistry();
				if (const ComponentInfo* info = componentRegistry.FindBySerializedName(scriptEntry.ClassName)) {
					if (info->isDynamic && info->add && (!info->has || !info->has(entity))) {
						info->add(entity);
						storageAdded = true;
					}
				}
			}

			if (alreadyHadScript && !storageAdded) {
				return false;
			}

			scene.MarkDirty();
			return true;
		}

		bool AttachManagedComponentToEntity(Entity entity, Scene& scene, const EditorScriptDiscovery::ScriptEntry& scriptEntry)
		{
			if (scriptEntry.ClassName.empty() || !scriptEntry.IsManagedComponent) {
				return false;
			}

			// Same teardown-race guard as AttachScriptToEntity above.
			if (!entity.IsValid() || !scene.IsValid(entity.GetHandle())) {
				return false;
			}

			if (!entity.HasComponent<ScriptComponent>()) {
				entity.AddComponent<ScriptComponent>();
			}

			auto& scriptComponent = entity.GetComponent<ScriptComponent>();
			if (scriptComponent.HasManagedComponent(scriptEntry.ClassName)) {
				return false;
			}

			scriptComponent.AddManagedComponent(scriptEntry.ClassName);
			scene.MarkDirty();
			return true;
		}

	} // namespace

	void RenderAddComponentPopup(
		const char* popupId,
		Scene& scene,
		std::span<const Entity> entities,
		char* searchBuffer,
		std::size_t searchBufferSize,
		bool* outChanged)
	{
		// Local helper: set the caller's "did we change anything" flag.
		// Cheaper than threading the pointer through every add site.
		auto markChanged = [&]() {
			if (outChanged) *outChanged = true;
		};
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const float availableH = viewport ? viewport->WorkSize.y : 800.0f;
		const float maxHeight = availableH * 0.7f;
		const float minHeight = std::min(maxHeight, std::max(360.0f, availableH * 0.45f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, minHeight),
			ImVec2(460.0f, maxHeight));

		if (!ImGui::BeginPopup(popupId)) {
			return;
		}

		// Defensive: an empty selection would make every "missing from any"
		// check vacuously true and offer the entire registry. The caller
		// shouldn't open the popup in that case, but be lenient.
		if (entities.empty()) {
			ImGui::TextDisabled("No entity selected.");
			ImGui::EndPopup();
			return;
		}

		const ComponentRegistry& registry = SceneManager::Get().GetComponentRegistry();

		Icons::TextIcon(Icons::Type::Search);
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##CompSearch", "Search components...",
			searchBuffer, searchBufferSize);
		ImGui::Separator();

		ImGui::BeginChild("##AddComponentScroll", ImVec2(0, 0), false,
			ImGuiWindowFlags_HorizontalScrollbar);

		std::string filter(searchBuffer);
		std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

		const bool hasFilter = !filter.empty();

		auto componentMissingFromAny = [&](const ComponentInfo& info) -> bool {
			for (const Entity& e : entities) {
				if (!info.has(e)) return true;
			}
			return false;
		};

		// nativeScriptByClassName MUST be defined before addComponentToAll lambda captures it.
		std::vector<EditorScriptDiscovery::ScriptEntry> scriptEntries;
		EditorScriptDiscovery::CollectProjectScriptEntries(scriptEntries);

		// Maps C++ display name → hand-authored mirror .cs entry so we can suppress duplicates in regular
		// subcategory buckets and render the "Native Components (C#)" tree under the C++ display name.
		// isDynamic components are NOT mirrors — their .cs IS the source of truth and routes by Subcategory.
		std::unordered_map<std::string, const EditorScriptDiscovery::ScriptEntry*> nativeBackingByDisplayName;
		for (const auto& scriptEntry : scriptEntries) {
			if (!scriptEntry.IsNativeComponent || scriptEntry.NativeName.empty()) continue;
			nativeBackingByDisplayName.emplace(scriptEntry.NativeName, &scriptEntry);
		}
		const auto isHandMirror = [&](const ComponentInfo& info) {
			if (info.isDynamic) return false; // dynamic components route by Subcategory
			return nativeBackingByDisplayName.find(info.displayName) != nativeBackingByDisplayName.end();
		};

		// Maps IComponent class name → script entry so addComponentToAll can route dynamic components
		// through the script-attach path (DynamicComponentRegistrar has no drawInspector/properties).
		std::unordered_map<std::string, const EditorScriptDiscovery::ScriptEntry*> nativeScriptByClassName;
		for (const auto& scriptEntry : scriptEntries) {
			if (!scriptEntry.IsNativeComponent) continue;
			if (scriptEntry.ClassName.empty()) continue;
			nativeScriptByClassName.emplace(scriptEntry.ClassName, &scriptEntry);
		}

		auto addComponentToAll = [&](const ComponentInfo& info) {
			bool added = false;
			if (info.isDynamic) {
				auto scriptIt = nativeScriptByClassName.find(info.displayName);
				if (scriptIt != nativeScriptByClassName.end() && scriptIt->second) {
					const auto& scriptEntry = *scriptIt->second;
					for (const Entity& e : entities) {
						if (AttachScriptToEntity(const_cast<Entity&>(e), scene, scriptEntry)) {
							added = true;
						}
					}
					if (added) markChanged();
					return;
				}
				for (const Entity& e : entities) {
					if (!info.has(e)) {
						info.add(e);
						added = true;
					}
				}
				if (added) {
					scene.MarkDirty();
					markChanged();
				}
				return;
			}
			for (const Entity& e : entities) {
				if (!info.has(e)) {
					registry.AddWithDependencies(e, info.typeId);
					added = true;
				}
			}
			if (added) {
				scene.MarkDirty();
				markChanged();
			}
		};

		auto componentConflictsWithSelection = [&](const std::type_index& proposed,
			std::string* outConflictName) -> bool
		{
			for (const Entity& e : entities) {
				if (registry.HasConflict(e, proposed)) {
					if (outConflictName) {
						registry.ForEachComponentInfo([&](const std::type_index& id, const ComponentInfo& info) {
							if (!outConflictName->empty()) return;
							if (id == proposed || !info.has || !info.has(e)) return;
							if (registry.TypesConflict(id, proposed)) *outConflictName = info.displayName;
						});
					}
					return true;
				}
			}
			return false;
		};

		if (hasFilter) {
			// Flat filtered list when searching across both built-in components
			// and project scripts.
			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (info.category != ComponentCategory::Component) return;
				if (info.displayName == "Scripts") return;
				if (!componentMissingFromAny(info)) return;
				if (isHandMirror(info)) return;

				std::string lowerName = info.displayName;
				std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
				if (lowerName.find(filter) == std::string::npos) return;

				std::string conflictName;
				const bool conflicts = componentConflictsWithSelection(typeId, &conflictName);
				const bool enabled = !conflicts;
				if (ImGuiUtils::MenuItemEllipsis(info.displayName, info.displayName.c_str(), nullptr, false, enabled, 260.0f)) {
					if (enabled) {
						addComponentToAll(info);
						ImGui::CloseCurrentPopup();
					}
				}
				if (conflicts && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip("Conflicts with %s", conflictName.c_str());
				}
			});

			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (info.category != ComponentCategory::Component) return;
				if (info.displayName == "Scripts") return;
				if (!componentMissingFromAny(info)) return;
				if (!isHandMirror(info)) return;

				std::string lowerName = info.displayName;
				std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
				if (lowerName.find(filter) == std::string::npos) return;

				std::string conflictName;
				const bool conflicts = componentConflictsWithSelection(typeId, &conflictName);
				const bool enabled = !conflicts;
				if (ImGuiUtils::MenuItemEllipsis(info.displayName, info.displayName.c_str(), nullptr, false, enabled, 260.0f)) {
					if (enabled) {
						addComponentToAll(info);
						ImGui::CloseCurrentPopup();
					}
				}
				if (conflicts && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip("Conflicts with %s", conflictName.c_str());
				}
			});

			for (const auto& scriptEntry : scriptEntries) {
				if (scriptEntry.IsSceneScript || scriptEntry.IsGlobalScript) {
					continue;
				}
				if (scriptEntry.IsNativeComponent) {
					continue;
				}
				std::string lowerClassName = EditorScriptDiscovery::ToLowerCopy(scriptEntry.ClassName);
				std::string lowerPath = EditorScriptDiscovery::ToLowerCopy(scriptEntry.Path.string());
				if (lowerClassName.find(filter) == std::string::npos
					&& lowerPath.find(filter) == std::string::npos) {
					continue;
				}

				const std::string label = BuildScriptMenuLabel(scriptEntry);
				const std::string path = scriptEntry.Path.string();
				if (ImGuiUtils::MenuItemEllipsis(label, path.c_str(), nullptr, false, true, 260.0f)) {
					bool added = false;
					for (const Entity& e : entities) {
						if (scriptEntry.IsManagedComponent) {
							added |= AttachManagedComponentToEntity(const_cast<Entity&>(e), scene, scriptEntry);
						}
						else {
							added |= AttachScriptToEntity(const_cast<Entity&>(e), scene, scriptEntry);
						}
					}
					if (added) markChanged();
					ImGui::CloseCurrentPopup();
				}
			}
		}
		else {
			// Categorized tree view. Subcategory order is fixed so users learn
			// muscle memory; unknown subcategories append after the known ones.
			struct CategoryEntry { std::type_index TypeId; const ComponentInfo* Info; };
			std::vector<std::pair<std::string, std::vector<CategoryEntry>>> categories;
			std::unordered_map<std::string, size_t> categoryIndex;

			const std::vector<std::string> subcategoryOrder = {
				"General", "Rendering", "Physics", "Audio"
			};
			for (const auto& sub : subcategoryOrder) {
				categoryIndex[sub] = categories.size();
				categories.emplace_back(sub, std::vector<CategoryEntry>{});
			}

			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (info.category != ComponentCategory::Component) return;
				if (info.displayName == "Scripts") return;
				if (!componentMissingFromAny(info)) return;
				if (isHandMirror(info)) return;

				std::string sub = info.subcategory.empty() ? "General" : info.subcategory;
				auto it = categoryIndex.find(sub);
				if (it == categoryIndex.end()) {
					categoryIndex[sub] = categories.size();
					categories.emplace_back(sub, std::vector<CategoryEntry>{});
					it = categoryIndex.find(sub);
				}
				categories[it->second].second.push_back({ typeId, &info });
			});

			for (const auto& [subcategory, components] : categories) {
				if (components.empty()) continue;

				if (ImGui::TreeNode(subcategory.c_str())) {
					for (const auto& entry : components) {
						const ComponentInfo* info = entry.Info;
						std::string conflictName;
						const bool conflicts = componentConflictsWithSelection(entry.TypeId, &conflictName);
						const bool enabled = !conflicts;
						if (ImGuiUtils::MenuItemEllipsis(info->displayName, info->displayName.c_str(), nullptr, false, enabled, 260.0f)) {
							if (enabled) {
								addComponentToAll(*info);
								ImGui::CloseCurrentPopup();
							}
						}
						if (conflicts && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
							ImGui::SetTooltip("Conflicts with %s", conflictName.c_str());
						}
					}
					ImGui::TreePop();
				}
			}

			// Native Components (C#) — C# IComponent structs mirroring a C++ pool; clicks route through
			// AddWithDependencies (NOT AddManagedComponent) since data lives in the C++ ECS pool.
			struct NativeEntry { std::type_index TypeId; const ComponentInfo* Info; };
			std::vector<NativeEntry> nativeBackedComponents;
			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (info.category != ComponentCategory::Component) return;
				if (info.displayName == "Scripts") return;
				if (!isHandMirror(info)) return;
				if (!componentMissingFromAny(info)) return;
				nativeBackedComponents.push_back({ typeId, &info });
			});
			if (!nativeBackedComponents.empty()) {
				if (ImGui::TreeNode("Native Components (C#)")) {
					for (const auto& entry : nativeBackedComponents) {
						const ComponentInfo* info = entry.Info;
						std::string conflictName;
						const bool conflicts = componentConflictsWithSelection(entry.TypeId, &conflictName);
						const bool enabled = !conflicts;
						if (ImGuiUtils::MenuItemEllipsis(info->displayName, info->displayName.c_str(), nullptr, false, enabled, 260.0f)) {
							if (enabled) {
								addComponentToAll(*info);
								ImGui::CloseCurrentPopup();
							}
						}
						if (conflicts && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
							ImGui::SetTooltip("Conflicts with %s", conflictName.c_str());
						}
					}
					ImGui::TreePop();
				}
			}

			bool hasManagedComponents = false;
			for (const auto& scriptEntry : scriptEntries) {
				if (scriptEntry.IsManagedComponent && !scriptEntry.IsSceneScript && !scriptEntry.IsGlobalScript) {
					hasManagedComponents = true;
					break;
				}
			}
			if (hasManagedComponents) {
				if (ImGui::TreeNode("Components (C#)")) {
					for (const auto& scriptEntry : scriptEntries) {
						if (!scriptEntry.IsManagedComponent || scriptEntry.IsSceneScript || scriptEntry.IsGlobalScript) {
							continue;
						}
						bool missingFromAny = false;
						for (const Entity& e : entities) {
							if (!e.HasComponent<ScriptComponent>()
								|| !e.GetComponent<ScriptComponent>().HasManagedComponent(scriptEntry.ClassName)) {
								missingFromAny = true;
								break;
							}
						}
						if (!missingFromAny) continue;
						const std::string label = BuildScriptMenuLabel(scriptEntry);
						const std::string path = scriptEntry.Path.string();
						if (ImGuiUtils::MenuItemEllipsis(label, path.c_str(), nullptr, false, true, 260.0f)) {
							bool added = false;
							for (const Entity& e : entities) {
								added |= AttachManagedComponentToEntity(const_cast<Entity&>(e), scene, scriptEntry);
							}
							if (added) markChanged();
							ImGui::CloseCurrentPopup();
						}
					}
					ImGui::TreePop();
				}
			}

			if (!scriptEntries.empty()) {
				if (ImGui::TreeNode("Scripts")) {
					for (const auto& scriptEntry : scriptEntries) {
						if (scriptEntry.IsManagedComponent || scriptEntry.IsNativeComponent
							|| scriptEntry.IsSceneScript || scriptEntry.IsGlobalScript) {
							continue;
						}
						const std::string label = BuildScriptMenuLabel(scriptEntry);
						const std::string path = scriptEntry.Path.string();
						if (ImGuiUtils::MenuItemEllipsis(label, path.c_str(), nullptr, false, true, 260.0f)) {
							bool added = false;
							for (const Entity& e : entities) {
								added |= AttachScriptToEntity(const_cast<Entity&>(e), scene, scriptEntry);
							}
							if (added) markChanged();
							ImGui::CloseCurrentPopup();
						}
					}
					ImGui::TreePop();
				}
			}
		}

		ImGui::EndChild();
		ImGui::EndPopup();
	}

}
