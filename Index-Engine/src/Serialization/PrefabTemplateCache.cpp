#include "pch.hpp"
#include "Serialization/PrefabTemplateCache.hpp"

#include "Core/Log.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Components/General/UUIDComponent.hpp"
#include "Components/General/PrefabInstanceComponent.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Components/Physics/BoxCollider2DComponent.hpp"
#include "Components/Physics/CircleCollider2DComponent.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/Entity.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/DynamicComponentStorage.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptType.hpp"
#include "Core/Application.hpp"

#if INDEX_WITH_EDITOR
#include "Serialization/FileWatcher.hpp"
#endif

#include <typeindex>
#include <utility>

namespace Index {

	namespace {

		void WalkTreeDFS(Scene& scene, EntityHandle root,
			std::vector<std::pair<EntityHandle, uint32_t>>& outOrdered)
		{
			auto recurse = [&](auto& self, EntityHandle handle, uint32_t parentIdx) -> void {
				const uint32_t myIdx = static_cast<uint32_t>(outOrdered.size());
				outOrdered.emplace_back(handle, parentIdx);
				Entity entity = scene.GetEntity(handle);
				for (EntityHandle child : entity.GetChildren()) {
					self(self, child, myIdx);
				}
			};
			recurse(recurse, root, UINT32_MAX);
		}

		std::string CaptureEntityName(Scene& scene, EntityHandle handle) {
			entt::registry& registry = scene.GetRegistry();
			if (const NameComponent* nc = registry.try_get<NameComponent>(handle)) {
				return nc->Name;
			}
			return {};
		}

		uint64_t CapturePersistentId(Scene& scene, EntityHandle handle) {
			return scene.GetEntityPersistentID(handle);
		}

		// Returns null when no Application/SceneManager exists (headless tools, doctest); callers fall back to the slow path.
		const ComponentRegistry* TryGetComponentRegistry() {
			Application* app = Application::GetInstance();
			if (!app || !app->GetSceneManager()) {
				return nullptr;
			}
			return &app->GetSceneManager()->GetComponentRegistry();
		}

		// A ScriptComponent is bakeable only when it is purely the inspector wrapper for C#
		// dynamic (IComponent) components: every listed class resolves to a dynamic component
		// and there are no behavior-script field values. Such a ScriptComponent carries no
		// data of its own — it is re-seeded at hydrate from the baked dynamic components.
		// A real managed/native behavior script (or any pending field values) makes it unbakeable.
		bool ScriptComponentIsPureDynamicSeed(const ScriptComponent& sc, const ComponentRegistry& registry) {
			if (!sc.PendingFieldValues.empty() || !sc.PendingDynamicComponentValues.empty()) {
				return false;
			}
			// Behavior scripts aren't in the ComponentRegistry, so FindBySerializedName returns
			// null for them -> not a pure seed -> unbakeable (correct). ASSUMPTION: a dynamic
			// component's serializedName (the C# struct's simple type name) never collides with a
			// behavior EntityScript's simple class name. A collision would resolve the script entry
			// to the dynamic component (isDynamic) and let this prefab bake, silently dropping the
			// behavior on the fast path only. Guard with ScriptEngine::IsScriptClass if that ever ships.
			for (const ScriptInstance& s : sc.Scripts) {
				const ComponentInfo* info = registry.FindBySerializedName(s.GetClassName());
				if (info == nullptr || !info->isDynamic) {
					return false;
				}
			}
			for (const std::string& className : sc.ManagedComponents) {
				const ComponentInfo* info = registry.FindBySerializedName(className);
				if (info == nullptr || !info->isDynamic) {
					return false;
				}
			}
			return true;
		}

	} // namespace

	PrefabTemplateCache& PrefabTemplateCache::Get()
	{
		static PrefabTemplateCache instance;
		return instance;
	}

	PrefabTemplateCache::~PrefabTemplateCache() {
#if INDEX_WITH_EDITOR
		m_Watcher.reset(); // joins watcher thread before remaining state is torn down
#endif
	}

	const PrefabTemplate* PrefabTemplateCache::Find(uint64_t prefabGuid) const
	{
		if (prefabGuid == 0) return nullptr;
		std::shared_lock lock(m_Mutex);
		auto it = m_Templates.find(prefabGuid);
		return (it != m_Templates.end()) ? it->second.get() : nullptr;
	}

	void PrefabTemplateCache::CaptureFromLive(uint64_t prefabGuid, Scene& scene,
		EntityHandle root, std::size_t fixupsAddedDuringDeserialize)
	{
		if (prefabGuid == 0 || root == entt::null) return;

		// No SceneManager (detached/headless): skip caching so the first spawn with a live manager bakes cleanly.
		const ComponentRegistry* componentRegistryPtr = TryGetComponentRegistry();
		if (componentRegistryPtr == nullptr) {
			return;
		}

		auto tmpl = std::make_unique<PrefabTemplate>();

		// Internal entity refs (deferred fixups) can't be represented in v1 templates; mark unbakeable so spawns fall back to the slow path.
		if (fixupsAddedDuringDeserialize > 0) {
			tmpl->bakeable = false;
			tmpl->unbakeableReason = "prefab contains internal entity references; "
				"baking requires template-local fixup capture (v2 follow-up)";
			std::unique_lock lock(m_Mutex);
			m_Templates[prefabGuid] = std::move(tmpl);
			return;
		}

		// 1) Depth-first walk: anchor parent indices and per-slot handles.
		std::vector<std::pair<EntityHandle, uint32_t>> ordered;
		WalkTreeDFS(scene, root, ordered);
		const uint32_t entityCount = static_cast<uint32_t>(ordered.size());
		tmpl->entities.resize(entityCount);

		// 2) Per-entity capture: name, source id, component byte slices.
		const ComponentRegistry& componentRegistry = *componentRegistryPtr;
		const entt::registry& registry = scene.GetRegistry();
		const std::type_index nameComponentTypeIndex = typeid(NameComponent);
		const std::type_index metaComponentTypeIndex = typeid(EntityMetaDataComponent);
		const std::type_index uuidComponentTypeIndex = typeid(UUIDComponent);
		const std::type_index prefabInstanceComponentTypeIndex = typeid(PrefabInstanceComponent);
		const std::type_index scriptComponentTypeIndex = typeid(ScriptComponent);
		const std::type_index hierarchyComponentTypeIndex = typeid(HierarchyComponent);

		for (uint32_t i = 0; i < entityCount; ++i) {
			if (!tmpl->bakeable) break;  // see post-loop reset; no point walking further

			EntityHandle h = ordered[i].first;
			PrefabTemplate::EntitySlot& slot = tmpl->entities[i];
			slot.parentIndex = ordered[i].second;
			slot.name = CaptureEntityName(scene, h);
			slot.sourceUuid = CapturePersistentId(scene, h);
			slot.componentRecordBegin = static_cast<uint32_t>(tmpl->components.size());

			// Identity components must never be copied from the source entity. Hydrate stamps
			// every instance with fresh metadata/UUID and rebuilds PrefabInstanceComponent from
			// that metadata. Copying the source UUID made every fast-spawned instance share one
			// script ID, so a physics hit could resolve to an unrelated prefab instance.
			componentRegistry.ForEachComponentInfo(
				[&](const std::type_index& typeId, const ComponentInfo& info) {
					if (!tmpl->bakeable) return;
					if (typeId == nameComponentTypeIndex) return;
					if (typeId == metaComponentTypeIndex) return;
					if (typeId == uuidComponentTypeIndex) return;
					if (typeId == prefabInstanceComponentTypeIndex) return;
					// Hierarchy is structural, not data: WalkTreeDFS already captured the parent
					// indices and HydrateInto rebuilds the links via SetParent. Its Children/Parent
					// are bake-tree EntityHandles that would be meaningless (and unbakeable — owns a
					// std::vector) if byte-copied, so exclude it like Name/Metadata.
					if (typeId == hierarchyComponentTypeIndex) return;
					if (info.has == nullptr || !info.has(scene.GetEntity(h))) return;

					// ScriptComponent is the inspector wrapper for C# dynamic components and owns
					// no bakeable bytes. Drop it when it only seeds dynamic components (re-seeded at
					// hydrate); a real behavior script makes the prefab unbakeable.
					if (typeId == scriptComponentTypeIndex) {
						if (!ScriptComponentIsPureDynamicSeed(
								scene.GetComponent<ScriptComponent>(h), componentRegistry)) {
							tmpl->bakeable = false;
							tmpl->unbakeableReason = "prefab entity has a behavior script ('Scripts'); "
								"behavior scripts can't be baked into the fast-spawn template. Use "
								"Entity.Instantiate for this prefab, or move the behavior into a system";
						}
						return;
					}

					// C# dynamic (IComponent) components have no writeBytes function pointer — it
					// can't capture the runtime storage — but their bytes live in dynamicStorage.
					// Capture them directly so prefabs carrying C# data components stay bakeable;
					// hydrate restores them through the existing emplaceFromBytes.
					if (info.isDynamic) {
						if (info.dynamicStorage == nullptr || info.typeIdU32 == 0) {
							tmpl->bakeable = false;
							tmpl->unbakeableReason = "dynamic component '" +
								(info.serializedName.empty() ? info.displayName : info.serializedName) +
								"' has no storage or stable type id; cannot bake";
							return;
						}
						const uint32_t byteOffset = static_cast<uint32_t>(tmpl->payloadBlob.size());
						const uint32_t byteSize = info.dynamicStorage->ElementSize();
						// RegisterDynamic refuses size 0, so ElementSize() is always >= 1; the guard
						// is belt-and-suspenders (Get on a 0-size storage would be ill-defined).
						if (byteSize > 0) {
							const void* raw = info.dynamicStorage->Get(h);
							if (raw == nullptr) return; // has() true but no row = storage drift; skip
							const uint8_t* bytes = static_cast<const uint8_t*>(raw);
							tmpl->payloadBlob.insert(tmpl->payloadBlob.end(), bytes, bytes + byteSize);
						}
						tmpl->components.push_back(
							PrefabTemplate::ComponentRecord{ info.typeIdU32, byteOffset, byteSize });
						return;
					}

					if (info.writeBytes == nullptr || info.typeIdU32 == 0) {
						tmpl->bakeable = false;
						tmpl->unbakeableReason = "component '" +
							(info.serializedName.empty() ? info.displayName : info.serializedName) +
							"' has no writeBytes callback or stable type id; cannot bake "
							"(component is not trivially-destructible — register a custom "
							"writeBytes/emplaceFromBytes that handles its heap-owned state, "
							"or accept the slow-path prefab spawn for this prefab)";
						return;
					}

					const uint32_t byteOffset =
						static_cast<uint32_t>(tmpl->payloadBlob.size());
					if (!info.writeBytes(registry, h, tmpl->payloadBlob)) {
						// has() true but writeBytes false = EnTT-storage drift; skip silently, component absent at hydrate.
						return;
					}
					const uint32_t byteSize =
						static_cast<uint32_t>(tmpl->payloadBlob.size() - byteOffset);
					tmpl->components.push_back(
						PrefabTemplate::ComponentRecord{ info.typeIdU32, byteOffset, byteSize });
				});

			slot.componentRecordEnd = static_cast<uint32_t>(tmpl->components.size());
		}

		if (!tmpl->bakeable) {
			tmpl->entities.clear();
			tmpl->components.clear();
			tmpl->payloadBlob.clear();
			IDX_CORE_WARN_TAG("PrefabTemplateCache",
				"Marking prefab {} as unbakeable: {}", prefabGuid, tmpl->unbakeableReason);
		}

		std::unique_lock lock(m_Mutex);
		m_Templates[prefabGuid] = std::move(tmpl);
	}

	EntityHandle PrefabTemplateCache::Hydrate(uint64_t prefabGuid, Scene& scene)
	{
		// Read lock held only across the pointer snapshot, not the entire hydrate.
		const PrefabTemplate* tmpl = nullptr;
		{
			std::shared_lock lock(m_Mutex);
			auto it = m_Templates.find(prefabGuid);
			if (it == m_Templates.end()) return entt::null;
			tmpl = it->second.get();
		}
		if (tmpl == nullptr || !tmpl->bakeable) return entt::null;

		const uint32_t entityCount = tmpl->EntityCount();
		if (entityCount == 0) return entt::null;

		// Self-contained variant: bulk-allocate, wrap in LoadGuard, then
		// delegate the per-component work to HydrateInto.
		scene.ReserveForLoadRuntime(entityCount, {});
		std::vector<EntityHandle> handles(entityCount);
		scene.CreateEntitiesBulk(entityCount, std::span<EntityHandle>(handles));

		Scene::LoadGuard guard(scene);
		EntityHandle root = HydrateInto(prefabGuid, scene,
			std::span<const EntityHandle>(handles));
		scene.MarkAllDirtyOnce();
		return root;
	}

	EntityHandle PrefabTemplateCache::HydrateInto(uint64_t prefabGuid, Scene& scene,
		std::span<const EntityHandle> preAllocated)
	{
		const PrefabTemplate* tmpl = nullptr;
		{
			std::shared_lock lock(m_Mutex);
			auto it = m_Templates.find(prefabGuid);
			if (it == m_Templates.end()) return entt::null;
			tmpl = it->second.get();
		}
		if (tmpl == nullptr || !tmpl->bakeable) return entt::null;

		const uint32_t entityCount = tmpl->EntityCount();
		if (entityCount == 0) return entt::null;
		if (preAllocated.size() != entityCount) {
			IDX_CORE_WARN_TAG("PrefabTemplateCache",
				"HydrateInto called with {} slots but template has {}; refusing to hydrate prefab {}",
				preAllocated.size(), entityCount, prefabGuid);
			return entt::null;
		}

		// Mirrors CaptureFromLive's guard; a bakeable template presupposes an Application, but guard anyway to avoid assert.
		const ComponentRegistry* componentRegistryPtr = TryGetComponentRegistry();
		if (componentRegistryPtr == nullptr) {
			return entt::null;
		}
		entt::registry& registry = scene.GetRegistry();
		const ComponentRegistry& componentRegistry = *componentRegistryPtr;

		// Identity stamping + name. Origin::Prefab + the prefab's GUID so
		// downstream queries (Entity.IsPrefabInstance, etc.) return the
		// same shape they would on the slow path.
		const AssetGUID prefabAssetGuid{ prefabGuid };
		for (uint32_t i = 0; i < entityCount; ++i) {
			scene.SetEntityMetaDataNoFlags(preAllocated[i], EntityOrigin::Prefab, prefabAssetGuid);
			registry.emplace_or_replace<NameComponent>(preAllocated[i], tmpl->entities[i].name);
		}

		for (uint32_t i = 0; i < entityCount; ++i) {
			const PrefabTemplate::EntitySlot& slot = tmpl->entities[i];
			for (uint32_t r = slot.componentRecordBegin; r < slot.componentRecordEnd; ++r) {
				const PrefabTemplate::ComponentRecord& rec = tmpl->components[r];
				const ComponentInfo* info = componentRegistry.GetByTypeId(rec.typeIdU32);
				if (info == nullptr || info->emplaceFromBytes == nullptr) {
					IDX_CORE_WARN_TAG("PrefabTemplateCache",
						"Skipping component record typeId={} during hydrate of prefab {}: "
						"no emplaceFromBytes registered",
						rec.typeIdU32, prefabGuid);
					continue;
				}
				info->emplaceFromBytes(registry, preAllocated[i],
					tmpl->payloadBlob.data() + rec.byteOffset, rec.byteSize);

				// Mirror seedScriptsEntryForDynamic: surface C# dynamic components in the
				// inspector's Scripts list, matching what the slow Instantiate path produces.
				if (info->isDynamic) {
					ScriptComponent& sc = registry.get_or_emplace<ScriptComponent>(preAllocated[i]);
					sc.AddScript(info->serializedName, ScriptType::Managed);
				}
			}
		}

		// Fast-spawn created the collider shapes with default material/geometry — CreateShape
		// ignores the component's logical state — so push the byte-restored state (size/radius,
		// center, friction/bounciness/layer, sensor, contact events) into the live shapes,
		// matching the slow Instantiate path. Runs after the construct hooks built the shapes,
		// before the mass re-assert (a resized or sensor-recreated shape changes mass).
		for (uint32_t i = 0; i < entityCount; ++i) {
			EntityHandle h = preAllocated[i];
			if (BoxCollider2DComponent* box = registry.try_get<BoxCollider2DComponent>(h)) {
				box->RestoreShapeFromState(scene);
			}
			else if (CircleCollider2DComponent* circle = registry.try_get<CircleCollider2DComponent>(h)) {
				circle->RestoreShapeFromState(scene);
			}
		}

		// Rigidbody state restore: push byte-restored gravity scale / freeze locks / pinned mass into
		// the live body (the construct hook made it with defaults; body type is honored by the hook).
		// Runs after the collider pass so the mass re-assert sees the final shapes.
		for (uint32_t i = 0; i < entityCount; ++i) {
			if (Rigidbody2DComponent* rb = registry.try_get<Rigidbody2DComponent>(preAllocated[i])) {
				rb->RestoreBodyFromState();
				// TEMP DIAGNOSTIC: a rigidbody and its sibling collider MUST share one b2 body.
				// If shared==false the ECB hydrate adoption failed (shape on one body, rigidbody on
				// another) — exactly the symptom where a spawned asteroid's collider sits at the origin
				// while its rigidbody/transform are at spawnPos. Remove once root-caused.
				uint64_t rbBody = b2StoreBodyId(rb->GetBodyHandle());
				uint64_t colBody = 0;
				if (auto* cc = registry.try_get<CircleCollider2DComponent>(preAllocated[i])) colBody = b2StoreBodyId(cc->m_BodyId);
				else if (auto* bc = registry.try_get<BoxCollider2DComponent>(preAllocated[i])) colBody = b2StoreBodyId(bc->m_BodyId);
				if (colBody != 0) {
					IDX_CORE_INFO_TAG("HydrateDbg", "ent={} rbBody={} colBody={} shared={} rbValid={}",
						static_cast<uint32_t>(preAllocated[i]), rbBody, colBody, (rbBody == colBody) ? 1 : 0, rb->IsValid() ? 1 : 0);
				}
			}
		}

		// Second pass: every entity must exist before SetParent runs so cycle guards and child-list updates resolve correctly.
		for (uint32_t i = 0; i < entityCount; ++i) {
			const uint32_t parentIdx = tmpl->entities[i].parentIndex;
			if (parentIdx == UINT32_MAX) continue;
			Entity child  = scene.GetEntity(preAllocated[i]);
			Entity parent = scene.GetEntity(preAllocated[parentIdx]);
			child.SetParent(parent);
		}

		return preAllocated[0];
	}

	void PrefabTemplateCache::Invalidate(uint64_t prefabGuid)
	{
		std::unique_lock lock(m_Mutex);
		m_Templates.erase(prefabGuid);
	}

	void PrefabTemplateCache::InvalidateAll()
	{
		std::unique_lock lock(m_Mutex);
		m_Templates.clear();
	}

	std::size_t PrefabTemplateCache::Size() const
	{
		std::shared_lock lock(m_Mutex);
		return m_Templates.size();
	}

#if INDEX_WITH_EDITOR
	void PrefabTemplateCache::InitializeForProject(const std::string& assetsRoot)
	{
		if (assetsRoot.empty()) {
			IDX_CORE_WARN_TAG("PrefabTemplateCache",
				"InitializeForProject called with empty assetsRoot; cache will not auto-invalidate on prefab edits");
			return;
		}

		// reset() joins the prior watcher thread before replacing it.
		m_Watcher = std::make_unique<FileWatcher>();
		m_Watcher->Watch(
			assetsRoot,
			".prefab",
			[]() {
				// Coarse invalidation on any .prefab edit; finer-grained path tracking requires watcher API changes.
				PrefabTemplateCache::Get().InvalidateAll();
			},
			/*recursive=*/true);
	}

	void PrefabTemplateCache::Shutdown()
	{
		m_Watcher.reset();
		InvalidateAll();
	}
#endif

} // namespace Index
