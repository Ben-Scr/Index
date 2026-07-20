#pragma once

#include "Inspector/PropertyDescriptor.hpp"
#include "Scene/ComponentCategory.hpp"
#include "Scene/Entity.hpp"
#include "Serialization/Json.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <typeindex>
#include <vector>

namespace Index::Serialization { class IArchive; }

namespace Index {

	class DynamicComponentStorage;

	struct ComponentInfo {
		std::string displayName;
		std::string serializedName;
		std::string subcategory;
		ComponentCategory category;

		ComponentInfo() = default;
		ComponentInfo(const std::string& displayName, ComponentCategory category)
			: displayName(displayName), category(category) {
		}
		ComponentInfo(const std::string& displayName, const std::string& subcategory, ComponentCategory category)
			: displayName(displayName), subcategory(subcategory), category(category) {
		}

		// Set by ComponentRegistry::Register; lets ComponentInfo* holders re-enter typed APIs without re-iterating the registry.
		std::type_index typeId{ typeid(void) };

		// FNV-1a 64-bit hash of serializedName; stable across builds so it can be embedded in binary assets. Zero = not yet hashed.
		uint64_t serializedNameHash = 0;

		// std::function (not raw ptr) so dynamic registrations can capture their DynamicComponentStorage; non-capturing lambdas from Register<T> avoid heap allocation via SBO.
		std::function<bool(Entity)> has;
		std::function<void(Entity)> add;
		std::function<void(Entity)> remove;
		std::function<void(Entity src, Entity dst)> copyTo;

		// Raw component pointer for the ScriptCore ref API (C# reads/writes fields via void* with no P/Invoke). Invalidated by the next add/remove on the pool — do not cache across frames.
		std::function<void*(Entity)> getRaw;

		// Bulk pointer fill for IJobQuery; returns total count so the managed caller can retry with a larger buffer if count > maxRows.
		std::function<int(entt::registry& registry, void** outPointers, int maxRows, int enableFilter)> fillRawPointers;

		// sizeof(T); mismatching the C# struct mirror hard-fails assembly load to prevent silent memory corruption.
		size_t rawSize = 0;

		// Monotonic u32 ID for the ECB wire format (4-byte component name). NOT persisted — scenes key on serializedName. Zero = unassigned.
		uint32_t typeIdU32 = 0;

		// entt::type_hash<T> for registry.storage(id) lookup; enables O(component count) iteration in script bindings, matching typed views.
		entt::id_type storageHash = 0;

		// Non-null for RegisterDynamic components (C# native). registry.storage(storageHash) returns nullptr for them; IJobQuery uses this pointer for O(1) access (~8x faster on 40k entities). Lifetime owned by ComponentRegistry::m_dynamicStorages.
		DynamicComponentStorage* dynamicStorage = nullptr;

		// ECB playback writes the raw payload in one call. Components with scene-bound runtime state (e.g. ParticleSystem2DComponent) must override to rebind, same as copyTo.
		std::function<void(entt::registry& registry, EntityHandle entity,
			const void* bytes, size_t size)> emplaceFromBytes;

		// Default-constructs via registry.emplace<T>, firing C++ default-initializers (e.g. Scale={1,1}). MUST NOT route through emplaceFromBytes — managed default(T) is all-zero and would clobber those defaults.
		std::function<void(entt::registry& registry, EntityHandle entity)> defaultEmplace;

		// Appends bytes to `out`; used by PrefabTemplateCache to bake a memcpy-ready blob.
		// std::function (not a raw pointer) so helpers like RegisterPrefixByteEmplacer can capture a
		// prefix size. Components owning non-trivial heap state (std::vector, non-SSO std::string) MUST
		// override — the default memcpy aliases freed memory after the source entity is destroyed.
		std::function<bool(const entt::registry& registry, EntityHandle entity,
			std::vector<uint8_t>& out)> writeBytes;

		// Fired by AddWithDependencies AFTER add; not called from raw add/deserialize/scene-load (those don't want smart defaults).
		void (*onAdd)(Entity) = nullptr;
		// Inspector draw for all selected entities. Prefer populating `properties` (auto-drawer); set this directly only for custom UX. drawInspector wins if both are set.
		void (*drawInspector)(std::span<const Entity>) = nullptr;

		// Per-component viewport gizmo called inside the Editor View gizmo pass. Must be safe to call when sibling components are absent.
		void (*drawEditorGizmo)(Entity) = nullptr;
		// When true, drawEditorGizmo runs for EVERY entity that has the component while
		// editor gizmos are enabled (always-visible, like Camera), not just the selected
		// ones. When false (default) the gizmo is drawn only for selected entities.
		bool drawEditorGizmoAlways = false;

		std::vector<PropertyDescriptor> properties;

		// Implicitly symmetric — declaring on either side is enough; HasConflict checks both directions.
		std::vector<std::type_index> conflictsWith;

		// Directed (not symmetric), walked transitively by AddWithDependencies. Conflict violations are skipped with a warning, never fail the parent add.
		std::vector<std::type_index> dependsOn;

		// Legacy JSON-tree path. SCHEDULED FOR REMOVAL — new components must use serializeArchive instead.
		// Package components must set both to round-trip in .scene/.prefab files.
		std::function<Json::Value(Entity)> serialize;
		std::function<void(Entity, const Json::Value&)> deserialize;

		// Unified binary+JSON path; wins over the legacy pair when both are set. Bump serializationVersion only for non-additive schema changes.
		void (*serializeArchive)(Entity, Index::Serialization::IArchive&) = nullptr;
		std::uint16_t serializationVersion = 1;

		// True for RegisterDynamic components (C# user assemblies); UnregisterAllDynamic drops all flagged entries on unload.
		bool isDynamic = false;
	};

} // namespace Index
