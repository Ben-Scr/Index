#include "pch.hpp"
#include "Scripting/EntityCommandBufferWire.hpp"
#include "Scripting/ScriptGlue.hpp"
#include "Scripting/ScriptBindings.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Serialization/PrefabTemplateCache.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Core/Application.hpp"
#include "Core/Log.hpp"

#include <cstring>
#include <span>
#include <vector>

namespace Index {

	static Scene* EcbResolveScene()
	{
		Scene* scene = ScriptEngine::GetScene();
		if (scene && scene->IsLoaded()) {
			return scene;
		}
		auto* app = Application::GetInstance();
		if (app && app->GetSceneManager()) {
			return app->GetSceneManager()->GetActiveScene();
		}
		return nullptr;
	}

	// memcpy to dodge unaligned-load UB: ECB stream is byte-packed, fields land on odd boundaries; compiler folds to a single load on x86/x64.
	template<typename T>
	static T EcbReadLE(const uint8_t* src) {
		T value;
		std::memcpy(&value, src, sizeof(T));
		return value;
	}

	static uint32_t Index_Component_GetTypeId(const char* componentName)
	{
		if (componentName == nullptr || componentName[0] == '\0') {
			return 0u;
		}
		const auto& registry = SceneManager::Get().GetComponentRegistry();
		if (const ComponentInfo* hit = registry.FindBySerializedName(componentName)) {
			return hit->typeIdU32;
		}
		constexpr const char kSuffix[] = "Component";
		constexpr std::size_t kSuffixLen = sizeof(kSuffix) - 1;
		const std::size_t nameLen = std::strlen(componentName);
		if (nameLen > kSuffixLen) {
			if (std::memcmp(componentName + nameLen - kSuffixLen, kSuffix, kSuffixLen) == 0) {
				std::string stem(componentName, nameLen - kSuffixLen);
				if (const ComponentInfo* hit = registry.FindBySerializedName(stem)) {
					return hit->typeIdU32;
				}
			}
		}
		uint32_t found = 0u;
		registry.ForEachComponentInfo([&](const std::type_index&, const ComponentInfo& info) {
			if (found != 0u) return;
			if (info.displayName == componentName) {
				found = info.typeIdU32;
			}
		});
		return found;
	}

	// Reused scratch vector: avoids per-call alloc in steady-state spawn loops. ECB playback is main-thread only.
	static std::vector<EntityHandle>& EcbHandleScratch()
	{
		static std::vector<EntityHandle> scratch;
		return scratch;
	}

	static int Index_Ecb_PlaybackImpl(const uint8_t* buffer, int length,
		uint64_t* outRuntimeIds, int maxOut)
	{
		if (buffer == nullptr || length < static_cast<int>(sizeof(EcbHeader))) {
			return kEcbErrorTruncated;
		}
		Scene* scene = EcbResolveScene();
		if (scene == nullptr) {
			return kEcbErrorNoScene;
		}

		EcbHeader header;
		std::memcpy(&header, buffer, sizeof(EcbHeader));

		// Verify the entity table fits before we trust entityCount.
		const std::size_t entityTableBytes =
			static_cast<std::size_t>(header.entityCount) * sizeof(uint32_t);
		if (static_cast<std::size_t>(length) < sizeof(EcbHeader) + entityTableBytes) {
			return kEcbErrorTruncated;
		}
		if (outRuntimeIds == nullptr && header.entityCount > 0) {
			return kEcbErrorOutputTooSmall;
		}
		// Reject negative maxOut explicitly: casting it to uint32_t below would wrap to
		// a huge value, defeating the capacity guard and allowing an OOB write into a
		// caller buffer that was declared negative-sized.
		if (maxOut < 0) {
			return kEcbErrorOutputTooSmall;
		}
		if (static_cast<uint32_t>(maxOut) < header.entityCount) {
			return kEcbErrorOutputTooSmall;
		}

		const uint8_t* entityTablePtr = buffer + sizeof(EcbHeader);
		const uint8_t* commandStreamPtr = entityTablePtr + entityTableBytes;
		const uint8_t* bufferEnd = buffer + length;

		// Pre-scan: bounds-validate all records and bake prefab templates in command-stream order so the execute pass can walk them by monotone index.
		PrefabTemplateCache& prefabCache = PrefabTemplateCache::Get();
		struct PrefabSpawn {
			uint32_t rootEntityIndex;        // ECB-local root slot
			uint64_t prefabGuid;
			uint32_t childBaseIndex;         // filled in after bulk-create sizing
			const PrefabTemplate* templ;
		};
		std::vector<PrefabSpawn> prefabSpawns;
		uint64_t totalExtraChildren = 0;

		{
			const uint8_t* cursor = commandStreamPtr;
			for (uint32_t cmdIdx = 0; cmdIdx < header.commandCount; ++cmdIdx) {
				if (cursor + 11 > bufferEnd) {
					return kEcbErrorTruncated;
				}
				const uint8_t  opcode      = cursor[0];
				const uint32_t entityIndex = EcbReadLE<uint32_t>(cursor + 1);
				const uint16_t payloadSize = EcbReadLE<uint16_t>(cursor + 9);
				const uint8_t* payload     = cursor + 11;
				if (payload + payloadSize > bufferEnd) {
					return kEcbErrorTruncated;
				}

				if (opcode == Ecb_InstantiatePrefab) {
					if (entityIndex >= header.entityCount || payloadSize != 8) {
						IDX_CORE_WARN_TAG("Ecb",
							"Malformed Ecb_InstantiatePrefab at cmd {} (entityIndex={}, payloadSize={})",
							cmdIdx, entityIndex, payloadSize);
						return kEcbErrorTruncated;
					}
					const uint64_t guid = EcbReadLE<uint64_t>(payload);
					const PrefabTemplate* templ = prefabCache.Find(guid);
					if (templ == nullptr) {
						// Bake-on-demand: instantiate + destroy a throwaway tree to populate the cache; cost paid once per GUID per process.
						EntityHandle scratchRoot = SceneSerializer::InstantiatePrefab(*scene, guid);
						if (scratchRoot != entt::null) {
							scene->DestroyEntity(scratchRoot);
						}
						templ = prefabCache.Find(guid);
					}
					if (templ == nullptr) {
						// Distinct from the unbakeable case: the GUID never resolved to a loadable
						// Prefab asset (SceneSerializer::InstantiatePrefab returned null), so no
						// template was baked. Entity.Instantiate(guid) would fail identically.
						IDX_CORE_WARN_TAG("Ecb",
							"Ecb_InstantiatePrefab rejected prefab {}: unknown prefab — the GUID does "
							"not resolve to a loadable Prefab asset (missing/unscanned .prefab or .meta, "
							"or a wrong/stale GUID). NOT a bake limitation; Entity.Instantiate would fail too.",
							guid);
						return kEcbErrorUnknownPrefab;
					}
					if (!templ->bakeable) {
						IDX_CORE_WARN_TAG("Ecb",
							"Ecb_InstantiatePrefab rejected prefab {}: {}",
							guid, templ->unbakeableReason);
						return kEcbErrorBadPrefab;
					}
					prefabSpawns.push_back({ entityIndex, guid, /*childBaseIndex=*/0u, templ });
					totalExtraChildren += static_cast<uint64_t>(templ->EntityCount() - 1u);
				}
				else if (opcode == Ecb_DestroyEntity) {
					const bool destroysLiveEntity = entityIndex == kEcbNoName;
					const bool validLiveDestroy = destroysLiveEntity && payloadSize == sizeof(uint64_t);
					const bool validLocalDestroy = !destroysLiveEntity
						&& entityIndex < header.entityCount
						&& payloadSize == 0;
					if (!validLiveDestroy && !validLocalDestroy) {
						IDX_CORE_WARN_TAG("Ecb",
							"Malformed Ecb_DestroyEntity at cmd {} (entityIndex={}, payloadSize={})",
							cmdIdx, entityIndex, payloadSize);
						return kEcbErrorTruncated;
					}
				}
				else if (opcode == Ecb_SetEntityEnabled) {
					const bool togglesLiveEntity = entityIndex == kEcbNoName;
					const bool validLiveToggle = togglesLiveEntity
						&& payloadSize == sizeof(uint8_t) + sizeof(uint64_t);
					const bool validLocalToggle = !togglesLiveEntity
						&& entityIndex < header.entityCount
						&& payloadSize == sizeof(uint8_t);
					if (!validLiveToggle && !validLocalToggle) {
						IDX_CORE_WARN_TAG("Ecb",
							"Malformed Ecb_SetEntityEnabled at cmd {} (entityIndex={}, payloadSize={})",
							cmdIdx, entityIndex, payloadSize);
						return kEcbErrorTruncated;
					}
				}

				cursor = payload + payloadSize;
			}
		}

		const uint64_t totalEntityCount64 =
			static_cast<uint64_t>(header.entityCount) + totalExtraChildren;

		// Guard against exceeding EnTT's entity_mask: registry.create() at the cap wraps the index and corrupts sparse-set writes.
		using EntityTraits = entt::entt_traits<EntityHandle>;
		constexpr uint64_t kMaxLiveEntities = static_cast<uint64_t>(EntityTraits::entity_mask);
		const uint64_t currentCount = static_cast<uint64_t>(scene->GetEntityCount());
		if (currentCount + totalEntityCount64 > kMaxLiveEntities) {
			IDX_CORE_ERROR_TAG("Ecb",
				"Ecb_Playback rejected: batch would exceed entity cap "
				"(current={}, requested={}+{} prefab children, cap={}). Raise 'entityBits' in "
				"project.json and rebuild (Project Settings > Entity ID bits).",
				currentCount, header.entityCount, totalExtraChildren, kMaxLiveEntities);
			return kEcbErrorEntityCapExceeded;
		}
		const uint32_t totalEntityCount = static_cast<uint32_t>(totalEntityCount64);

		// Root slots [0, entityCount); prefab children [entityCount, totalEntityCount). Only roots are surfaced to managed code.
		scene->ReserveForLoadRuntime(totalEntityCount, {});

		std::vector<EntityHandle>& handles = EcbHandleScratch();
		handles.resize(totalEntityCount);
		scene->CreateEntitiesBulk(totalEntityCount, handles);

		// Assign each prefab spawn its child block now that the layout
		// is known.
		{
			uint32_t base = header.entityCount;
			for (PrefabSpawn& spawn : prefabSpawns) {
				spawn.childBaseIndex = base;
				base += (spawn.templ->EntityCount() - 1u);
			}
		}

		Scene::LoadGuard guard(*scene);

		// Skip prefab roots: HydrateInto stamps them with Origin::Prefab; overwriting here would clobber the GUID.
		std::vector<uint8_t> isPrefabRoot(header.entityCount, 0);
		for (const PrefabSpawn& spawn : prefabSpawns) {
			isPrefabRoot[spawn.rootEntityIndex] = 1;
		}
		std::vector<uint8_t> destroyedRoot(header.entityCount, 0);
		for (uint32_t i = 0; i < header.entityCount; ++i) {
			if (isPrefabRoot[i] == 0) {
				scene->SetEntityMetaDataNoFlags(handles[i], EntityOrigin::Runtime);
			}
		}

		const ComponentRegistry& componentRegistry = SceneManager::Get().GetComponentRegistry();

		std::vector<EntityHandle> prefabSlotScratch;
		std::size_t nextPrefabSpawn = 0;

		// Dynamic IComponent adds must seed a ScriptComponent.Scripts entry so the inspector renders it; idempotent.
		auto seedScriptsEntryForDynamic = [&](const ComponentInfo* info, EntityHandle handle) {
			if (!info->isDynamic) return;
			Entity entity = scene->GetEntity(handle);
			if (!entity.HasComponent<ScriptComponent>()) {
				entity.AddComponent<ScriptComponent>();
			}
			auto& sc = entity.GetComponent<ScriptComponent>();
			if (!sc.HasScript(info->serializedName, ScriptType::Managed)) {
				sc.AddScript(info->serializedName, ScriptType::Managed);
			}
		};

		const uint8_t* cursor = commandStreamPtr;
		for (uint32_t cmdIdx = 0; cmdIdx < header.commandCount; ++cmdIdx) {
			if (cursor + 11 > bufferEnd) {
				return kEcbErrorTruncated;
			}
			const uint8_t  opcode       = cursor[0];
			const uint32_t entityIndex  = EcbReadLE<uint32_t>(cursor + 1);
			const uint32_t typeId       = EcbReadLE<uint32_t>(cursor + 5);
			const uint16_t payloadSize  = EcbReadLE<uint16_t>(cursor + 9);
			const uint8_t* payload      = cursor + 11;
			if (payload + payloadSize > bufferEnd) {
				return kEcbErrorTruncated;
			}
			cursor = payload + payloadSize;

			const bool targetsLiveEntity =
				(opcode == Ecb_DestroyEntity || opcode == Ecb_SetEntityEnabled)
				&& entityIndex == kEcbNoName;
			if (!targetsLiveEntity && entityIndex >= header.entityCount) {
				IDX_CORE_WARN_TAG("Ecb", "Skipping command with out-of-range entityIndex {}", entityIndex);
				continue;
			}

			if (opcode == Ecb_AddComponent || opcode == Ecb_SetComponent) {
				if (destroyedRoot[entityIndex] != 0) {
					continue;
				}
				const ComponentInfo* info = componentRegistry.GetByTypeId(typeId);
				if (info == nullptr || info->emplaceFromBytes == nullptr) {
					IDX_CORE_WARN_TAG("Ecb",
						"AddComponent for typeId {} cannot proceed: no registered "
						"component or component has no emplaceFromBytes callback "
						"(component holds non-memcpy-safe state and needs a "
						"custom emplacer — see ComponentRegistry contract).",
						typeId);
					return kEcbErrorUnknownComponent;
				}
				info->emplaceFromBytes(scene->GetRegistry(), handles[entityIndex],
					payload, payloadSize);
				seedScriptsEntryForDynamic(info, handles[entityIndex]);
			}
			else if (opcode == Ecb_DefaultConstructComponent) {
				if (destroyedRoot[entityIndex] != 0) {
					continue;
				}
				const ComponentInfo* info = componentRegistry.GetByTypeId(typeId);
				if (info == nullptr || info->defaultEmplace == nullptr) {
					IDX_CORE_WARN_TAG("Ecb",
						"DefaultConstructComponent for typeId {} cannot proceed: no "
						"registered component or component is not default-constructible "
						"(use the value or patch overload of AddComponent instead).",
						typeId);
					return kEcbErrorUnknownComponent;
				}
				info->defaultEmplace(scene->GetRegistry(), handles[entityIndex]);
				seedScriptsEntryForDynamic(info, handles[entityIndex]);
			}
			else if (opcode == Ecb_InstantiatePrefab) {
				if (nextPrefabSpawn >= prefabSpawns.size()) {
					IDX_CORE_ERROR_TAG("Ecb",
						"Ecb_InstantiatePrefab execute-pass iterator out of sync with pre-scan (cmd {})", cmdIdx);
					return kEcbErrorTruncated;
				}
				const PrefabSpawn& spawn = prefabSpawns[nextPrefabSpawn++];
				const PrefabTemplate& templ = *spawn.templ;
				const uint32_t childCount = templ.EntityCount();
				if (destroyedRoot[spawn.rootEntityIndex] != 0) {
					for (uint32_t k = 1; k < childCount; ++k) {
						EntityHandle child = handles[spawn.childBaseIndex + k - 1u];
						if (scene->IsValid(child)) {
							scene->DestroyEntity(child);
						}
					}
					continue;
				}

				prefabSlotScratch.resize(childCount);
				prefabSlotScratch[0] = handles[spawn.rootEntityIndex];
				for (uint32_t k = 1; k < childCount; ++k) {
					prefabSlotScratch[k] = handles[spawn.childBaseIndex + k - 1u];
				}

				prefabCache.HydrateInto(spawn.prefabGuid, *scene,
					std::span<const EntityHandle>(prefabSlotScratch));
			}
			else if (opcode == Ecb_DestroyEntity) {
				if (entityIndex == kEcbNoName) {
					const uint64_t entityId = EcbReadLE<uint64_t>(payload);
					EntityHandle resolved = entt::null;
					if (scene->TryResolveEntityRef(entityId, resolved)) {
						scene->DestroyEntity(resolved);
					}
				}
				else if (destroyedRoot[entityIndex] == 0) {
					destroyedRoot[entityIndex] = 1;
					if (scene->IsValid(handles[entityIndex])) {
						scene->DestroyEntity(handles[entityIndex]);
					}
				}
			}
			else if (opcode == Ecb_SetEntityEnabled) {
				// payload[0] = enabled flag; live entities carry the u64 persistent id after it.
				const bool enabled = payload[0] != 0;
				if (entityIndex == kEcbNoName) {
					const uint64_t entityId = EcbReadLE<uint64_t>(payload + 1);
					EntityHandle resolved = entt::null;
					if (scene->TryResolveEntityRef(entityId, resolved) && scene->IsValid(resolved)) {
						scene->GetEntity(resolved).SetEnabled(enabled);
					}
				}
				else if (destroyedRoot[entityIndex] == 0 && scene->IsValid(handles[entityIndex])) {
					scene->GetEntity(handles[entityIndex]).SetEnabled(enabled);
				}
			}
				// Unknown opcodes: skip (forwards-compat). We already advanced
			// `cursor` past the record's payload, so the stream stays
			// aligned for the next iteration.
		}

		// A Transform2D written by the ECB to place a spawned prefab (e.g.
		// ecb.AddComponent(root, new NativeTransform2D{Position})) is a raw byte copy. m_Dirty is set in
		// the bytes, but m_OwnerScene/m_OwnerEntity are zeroed, so MarkDirty can't enqueue the entity in
		// the scene dirty-transform set; and because the root already has a Transform2D from hydrate
		// this is an emplace_or_replace REPLACE (fires on_update, not on_construct), so no hook moves
		// the body the eager Rigidbody2D/collider construct hooks already created at the prefab origin.
		// Net: transform/sprite at spawnPos, body left at the origin. Re-stamp the owner + mark dirty
		// (ongoing sync + serialization), and teleport each root physics body straight to its transform
		// so the spawn position takes effect immediately.
		{
			entt::registry& reg = scene->GetRegistry();
			for (uint32_t i = 0; i < totalEntityCount; ++i) {
				const EntityHandle h = handles[i];
				if (!scene->IsValid(h)) {
					continue;
				}
				Transform2DComponent* tr = reg.try_get<Transform2DComponent>(h);
				if (tr == nullptr) {
					continue;
				}
				tr->BindOwner(scene, h);
				tr->MarkDirty();
				// Only roots have world == local now (child world transforms are composed later by the
				// hierarchy pass), so only teleport their body straight to the transform.
				const HierarchyComponent* hc = reg.try_get<HierarchyComponent>(h);
				const bool isRoot = (hc == nullptr) || hc->Parent == entt::null;
				if (Rigidbody2DComponent* rb = reg.try_get<Rigidbody2DComponent>(h); rb != nullptr) {
					const Vec2 bodyBefore = rb->GetPosition();
					const bool moved = isRoot && rb->IsValid();
					if (moved) {
						rb->SetTransform(*tr);
					}
					// TEMP DIAGNOSTIC: confirm the override landed (trPos==spawnPos) and the body actually
					// moved (bodyAfter==spawnPos). Remove once the ECB physics-spawn position is verified.
					IDX_CORE_INFO_TAG("EcbFixDbg",
						"ent={} isRoot={} rbValid={} moved={} trPos=({:.1f},{:.1f}) bodyBefore=({:.1f},{:.1f}) bodyAfter=({:.1f},{:.1f})",
						static_cast<uint32_t>(h), isRoot ? 1 : 0, rb->IsValid() ? 1 : 0, moved ? 1 : 0,
						tr->Position.x, tr->Position.y, bodyBefore.x, bodyBefore.y, rb->GetPosition().x, rb->GetPosition().y);
				}
			}
		}

		for (uint32_t i = 0; i < header.entityCount; ++i) {
			outRuntimeIds[i] = destroyedRoot[i] != 0
				? 0
				: scene->GetEntityPersistentID(handles[i]);
		}

		// Pulse lives outside the guard scope so it isn't double-counted by a later MarkAllDirtyOnce().
		scene->MarkAllDirtyOnce();
		return static_cast<int>(header.entityCount);
	}

	// Boundary guard: Ecb_Playback is reached via [UnmanagedCallersOnly]; an exception unwinding
	// into CoreCLR is UB / fatal. Playback allocates heavily (prefab bake, scratch vectors) and can
	// throw std::bad_alloc, so swallow here and surface as an error code the C# side already handles.
	static int Index_Ecb_Playback(const uint8_t* buffer, int length,
		uint64_t* outRuntimeIds, int maxOut)
	{
		try {
			return Index_Ecb_PlaybackImpl(buffer, length, outRuntimeIds, maxOut);
		}
		catch (const std::exception& e) {
			IDX_CORE_ERROR_TAG("ScriptBinding", "exception in Index_Ecb_Playback: {}", e.what());
			return kEcbErrorInternal;
		}
		catch (...) {
			IDX_CORE_ERROR_TAG("ScriptBinding", "unknown exception in Index_Ecb_Playback");
			return kEcbErrorInternal;
		}
	}

	void PopulateEcbBindings(NativeBindings& b)
	{
		b.Component_GetTypeId = &Index_Component_GetTypeId;
		b.Ecb_Playback        = &Index_Ecb_Playback;
	}

} // namespace Index
