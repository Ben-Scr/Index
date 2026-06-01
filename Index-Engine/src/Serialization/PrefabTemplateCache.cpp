#include "pch.hpp"
#include "Serialization/PrefabTemplateCache.hpp"

#include "Core/Log.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/Entity.hpp"
#include "Scene/ComponentRegistry.hpp"
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

		for (uint32_t i = 0; i < entityCount; ++i) {
			if (!tmpl->bakeable) break;  // see post-loop reset; no point walking further

			EntityHandle h = ordered[i].first;
			PrefabTemplate::EntitySlot& slot = tmpl->entities[i];
			slot.parentIndex = ordered[i].second;
			slot.name = CaptureEntityName(scene, h);
			slot.sourceUuid = CapturePersistentId(scene, h);
			slot.componentRecordBegin = static_cast<uint32_t>(tmpl->components.size());

			// NameComponent and EntityMetaDataComponent excluded: the cache owns their values; metadata gets a fresh RuntimeID at hydrate.
			componentRegistry.ForEachComponentInfo(
				[&](const std::type_index& typeId, const ComponentInfo& info) {
					if (!tmpl->bakeable) return;
					if (typeId == nameComponentTypeIndex) return;
					if (typeId == metaComponentTypeIndex) return;
					if (info.has == nullptr || !info.has(scene.GetEntity(h))) return;

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
