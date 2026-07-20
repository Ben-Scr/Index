#pragma once

#include "Inspector/PropertyDescriptor.hpp"
#include "Scene/ComponentCategory.hpp"
#include "Scene/ComponentInfo.hpp"
#include "Scene/SceneManager.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace Index::Codegen {

    template <typename T>
    void RegisterComponent(SceneManager& sceneManager,
                           const std::string& displayName,
                           ComponentCategory category = ComponentCategory::Component,
                           const std::string& subcategory = "",
                           const std::string& serializedName = "",
                           std::vector<PropertyDescriptor> properties = {}) {
        ComponentInfo info{ displayName, subcategory, category };
        info.serializedName = serializedName;
        info.properties = std::move(properties);
        sceneManager.RegisterComponentType<T>(info);
    }

    // Registers a byte-emplacer for a component whose C# native mirror is a
    // PREFIX of the larger C++ struct — i.e. the C++ type carries trailing
    // non-memcpy-safe state (std::string, owning containers) past the bytes
    // the managed side knows about. SpriteRendererComponent is the case:
    // NativeSpriteRenderer is the 40-byte POD prefix; SpriteName (std::string)
    // lives past it.
    //
    // The auto byte-emplacer in ComponentRegistry is gated on
    // is_trivially_destructible and skips these types, leaving emplaceFromBytes
    // null — so ECB AddComponent of the mirror throws "no native emplacer".
    // This sets a safe one: default-construct (constructing the trailing
    // members), copy only the wire bytes into the prefix, then move-emplace.
    // `prefixSize` is offsetof(T, firstNonMirrorMember) so a future field
    // added past the mirror can't be clobbered.
    template <typename T>
    void RegisterPrefixByteEmplacer(SceneManager& sceneManager, std::size_t prefixSize) {
        const std::type_index targetId(typeid(T));
        sceneManager.GetComponentRegistry().ForEachComponentInfo(
            [targetId, prefixSize](const std::type_index& id, ComponentInfo& info) {
                if (id != targetId) return;
                info.emplaceFromBytes = [prefixSize](entt::registry& registry, EntityHandle entity,
                                                     const void* bytes, std::size_t size) {
                    T component;  // constructs trailing non-POD members (e.g. std::string)
                    std::memcpy(&component, bytes, std::min(size, prefixSize));
                    registry.emplace_or_replace<T>(entity, std::move(component));
                };
                // Symmetric bake side. Without it the component has no writeBytes and every prefab
                // containing it is rejected as unbakeable by PrefabTemplateCache. Capture only the POD
                // prefix (never the trailing std::string's bytes — those are heap pointers that would
                // dangle); emplaceFromBytes reconstructs the trailing members, matching the hydrate side.
                info.writeBytes = [prefixSize](const entt::registry& registry, EntityHandle entity,
                                               std::vector<uint8_t>& out) -> bool {
                    const T* component = registry.try_get<T>(entity);
                    if (component == nullptr) return false;
                    const auto* bytes = reinterpret_cast<const uint8_t*>(component);
                    out.insert(out.end(), bytes, bytes + prefixSize);
                    return true;
                };
            });
    }

    // Adds both A→B and B→A to conflictsWith; ValidateConflictSymmetry asserts symmetry in debug.
    template <typename A, typename B>
    void DeclareConflict(SceneManager& sceneManager) {
        const std::type_index aId(typeid(A));
        const std::type_index bId(typeid(B));
        sceneManager.GetComponentRegistry().ForEachComponentInfo(
            [&](const std::type_index& id, ComponentInfo& info) {
                if (id == aId) {
                    bool present = false;
                    for (const auto& c : info.conflictsWith) if (c == bId) { present = true; break; }
                    if (!present) info.conflictsWith.push_back(bId);
                } else if (id == bId) {
                    bool present = false;
                    for (const auto& c : info.conflictsWith) if (c == aId) { present = true; break; }
                    if (!present) info.conflictsWith.push_back(aId);
                }
            });
    }

    // TDependent.dependsOn += TDependency. Directed (NOT symmetric):
    // the inverse declaration would mean "TDependency pulls in TDependent
    // on add," which is virtually never what we want.
    template <typename TDependent, typename TDependency>
    void DeclareDependency(SceneManager& sceneManager) {
        const std::type_index dependentId(typeid(TDependent));
        const std::type_index dependencyId(typeid(TDependency));
        sceneManager.GetComponentRegistry().ForEachComponentInfo(
            [&](const std::type_index& id, ComponentInfo& info) {
                if (id != dependentId) return;
                for (const auto& c : info.dependsOn) if (c == dependencyId) return;
                info.dependsOn.push_back(dependencyId);
            });
    }

}
