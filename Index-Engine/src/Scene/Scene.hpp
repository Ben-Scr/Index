#pragma once
#include "Scene/Entity.hpp"
#include "Scene/ISystem.hpp"
#include "Collections/Ids.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Core/Export.hpp"
#include "Core/UUID.hpp"
#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Index {
	class SceneDefinition;
	class Camera2DComponent;
	class Renderer2D;
	class Transform2DComponent;
	class TransformHierarchySystem;
	class INDEX_API Scene {
		friend class SceneManager;
		friend class SceneDefinition;
		friend class Application;
		friend class Renderer2D;
		friend class Entity;
		friend class Transform2DComponent;
		friend class TransformHierarchySystem;

	public:
		Scene(const Scene&) = delete;

		// Detached scene: no SceneDefinition, no tick, global subsystem hooks (physics/audio/scripts) gated off.
		static std::unique_ptr<Scene> CreateDetachedScene(const std::string& name);
		bool IsDetached() const { return m_IsDetached; }

		// Info: Creates an entity with a Transform2D component
		Entity CreateEntity();
		// Info: Creates an entity with a Transform2D and Name component
		Entity CreateEntity(const std::string& name);
		Entity CreateEmptyEntity();
		Entity CreateRuntimeEntity();
		Entity CreateRuntimeEntity(const std::string& name);
		Entity CreatePrefabInstance(AssetGUID prefabGuid, const std::string& name = "Entity");

		Entity CloneEntity(Entity source);
		// Info: Creates an entity handle and applies scene invariants such as UUID + dirty tracking
		EntityHandle CreateEntityHandle(
			EntityOrigin origin = EntityOrigin::Scene,
			AssetGUID prefabGuid = AssetGUID(0),
			AssetGUID sceneGuid = AssetGUID(0),
			EntityID runtimeId = 0);

		Entity GetEntity(EntityHandle nativeEntity) {
			IDX_CORE_ASSERT(IsValid(nativeEntity), IndexErrorCode::InvalidValue,
				"Scene::GetEntity called with an invalid/destroyed EntityHandle");
			return Entity(nativeEntity, *this);
		}
		[[deprecated("Returns a writable Entity from a const Scene; prefer Scene::GetComponent<T>(handle) const for read-only access")]]
		Entity GetEntity(EntityHandle nativeEntity) const {
			IDX_CORE_ASSERT(IsValid(nativeEntity), IndexErrorCode::InvalidValue,
				"Scene::GetEntity called with an invalid/destroyed EntityHandle");
			return Entity(nativeEntity, const_cast<Scene&>(*this));
		}

		void DestroyEntity(Entity entity);
		void DestroyEntity(EntityHandle nativeEntity);
		void DestroyEntity(EntityHandle nativeEntity, float delay);
		void ClearEntities();
		bool BeginContentReplacement();
		void EndContentReplacement(bool restartLifecycle);

		template <typename... TComponent>
		void ReserveForLoad(std::size_t entityCount) {
			m_Registry.storage<EntityHandle>().reserve(entityCount);
			((m_Registry.storage<TComponent>().reserve(entityCount)), ..., (void)0);
			m_RuntimeIdToEntity.reserve(entityCount);
			m_SceneGuidToEntity.reserve(entityCount);
		}

		// IMPORTANT: does NOT populate identity components — caller must call SetEntityMetaData on each handle.
		void CreateEntitiesBulk(std::size_t n, std::span<EntityHandle> out);

		void ReserveForLoadRuntime(std::size_t entityCount,
			std::span<const std::pair<uint32_t, std::size_t>> perTypeCounts);

		// Variant that skips m_Dirty / m_UIDirty — ECB playback calls MarkAllDirtyOnce() at batch end instead.
		void SetEntityMetaDataNoFlags(
			EntityHandle entity,
			EntityOrigin origin,
			AssetGUID prefabGuid = AssetGUID(0),
			AssetGUID sceneGuid = AssetGUID(0),
			EntityID runtimeId = 0);

		void MarkAllDirtyOnce();

		// Defers idempotent on_construct hooks until the outermost guard destructor; Physics/Camera/DisabledTag hooks fire immediately (they read sibling state not yet ready during bulk load).
		class INDEX_API LoadGuard {
		public:
			explicit LoadGuard(Scene& scene);
			~LoadGuard();
			LoadGuard(const LoadGuard&) = delete;
			LoadGuard& operator=(const LoadGuard&) = delete;
			LoadGuard(LoadGuard&&) = delete;
			LoadGuard& operator=(LoadGuard&&) = delete;
		private:
			Scene& m_Scene;
			bool m_OwnsSuppression;
		};

		bool IsConstructHooksSuppressed() const { return m_SuppressConstructHooks; }

		bool IsValid(EntityHandle nativeEntity) const {
			return m_Registry.valid(nativeEntity);
		}
		template<typename TComponent, typename... Args>
			requires (!std::is_empty_v<TComponent>)
		TComponent& AddComponent(EntityHandle entity, Args&&... args) {
			return ComponentUtils::AddComponent<TComponent>(m_Registry, entity, std::forward<Args>(args)...);
		}

		template<typename TTag>
			requires std::is_empty_v<TTag>
		void AddComponent(EntityHandle entity) {
			ComponentUtils::AddComponent<TTag>(m_Registry, entity);
		}

		template<typename TComponent>
		bool HasComponent(EntityHandle entity) const {
			return ComponentUtils::HasComponent<TComponent>(m_Registry, entity);
		}

		template<typename... TComponent>
		bool HasAnyComponent(EntityHandle entity) const {
			return ComponentUtils::HasAnyComponent<TComponent...>(m_Registry, entity);
		}

		template<typename... TComponent, typename... TEntity>
		bool AnyHasComponent(TEntity... handles) const {
			return ComponentUtils::AnyHasComponent<TComponent...>(m_Registry, handles...);
		}

		template<typename TComponent>
		TComponent& GetComponent(EntityHandle entity) {
			return ComponentUtils::GetComponent<TComponent>(m_Registry, entity);
		}

		template<typename TComponent>
		const TComponent& GetComponent(EntityHandle entity) const {
			return ComponentUtils::GetComponent<TComponent>(m_Registry, entity);
		}

		template<typename TComponent>
		bool TryGetComponent(EntityHandle entity, TComponent*& out) {
			out = ComponentUtils::TryGetComponent<TComponent>(m_Registry, entity);
			return out != nullptr;
		}

		template<typename TComponent>
		void RemoveComponent(EntityHandle entity) {
			ComponentUtils::RemoveComponent<TComponent>(m_Registry, entity);
		}

		template<typename TComponent>
		TComponent* GetSingletonComponent() {
			auto view = m_Registry.view<TComponent>();
			const auto count = view.size();
			if (count == 0) {
				IDX_CORE_ERROR_TAG("Scene", "Singleton component '{}' not found", typeid(TComponent).name());
				return nullptr;
			}
			if (count > 1) {
				IDX_CORE_WARN_TAG("Scene", "Multiple ({}) instances of singleton component '{}', returning first", count, typeid(TComponent).name());
			}

			auto entity = *view.begin();
			return &view.template get<TComponent>(entity);
		}

		template<typename TComponent>
		entt::entity GetSingletonEntity() {
			auto view = m_Registry.view<TComponent>();

			const auto count = view.size();
			if (count == 0) {
				IDX_CORE_ERROR_TAG("Scene", "Singleton entity with component '{}' not found", typeid(TComponent).name());
				return entt::null;
			}
			IDX_VERIFY(count == 1, IndexErrorCode::InvalidValue,
				"Multiple instances of singleton component '" + std::string(typeid(TComponent).name()) +
				"' (count=" + std::to_string(count) + "). Caller expects exactly one.");
			if (count > 1) {
				IDX_CORE_WARN_TAG("Scene", "Multiple ({}) instances of singleton component '{}', returning first", count, typeid(TComponent).name());
			}

			return *view.begin();
		}

		template<typename T>
		T* GetSystem() {
			static_assert(std::is_base_of<ISystem, T>::value, "T must derive from ISystem");

			for (auto& sysPtr : m_Systems) {
				if (auto ptr = dynamic_cast<T*>(sysPtr.get())) {
					return ptr;
				}
			}

			// Demoted from ERROR to TRACE: GetSystem<T>() is a soft probe and was
			// spamming the log when callers legitimately treat a missing system as
			// "not present, skip this work". Callers that REQUIRE the system to be
			// present should IDX_CORE_ASSERT on the result themselves.
			IDX_CORE_TRACE_TAG("Scene", "System not found: {}", typeid(T).name());
			return nullptr;
		}

		template<typename T>
		bool HasSystem() const {
			static_assert(std::is_base_of<ISystem, T>::value, "T must derive from ISystem");

			for (const auto& sysPtr : m_Systems) {
				if (dynamic_cast<T*>(sysPtr.get())) {
					return true;
				}
			}
			return false;
		}

		template<typename T>
		void DisableSystem() {
			static_assert(std::is_base_of<ISystem, T>::value, "T must derive from ISystem");

			for (auto& sysPtr : m_Systems) {
				if (auto ptr = dynamic_cast<T*>(sysPtr.get())) {
					if (ptr->IsEnabled()) {
						ptr->SetEnabled(false, *this);
					}
				}
			}
		}

		template<typename T>
		void EnableSystem() {
			static_assert(std::is_base_of<ISystem, T>::value, "T must derive from ISystem");

			for (auto& sysPtr : m_Systems) {
				if (auto ptr = dynamic_cast<T*>(sysPtr.get())) {
					ptr->SetEnabled(true, *this);
				}
			}
		}

		entt::registry& GetRegistry() { return m_Registry; }
		const entt::registry& GetRegistry() const { return m_Registry; }

		std::size_t GetEntityCount() const { return m_EntityCount; }

		const std::string& GetName() const { return m_Name; }
		void SetName(const std::string& name) { m_Name = name; }
		bool IsLoaded() const { return m_IsLoaded; }
		bool IsPersistent() const { return m_Persistent; }
		const SceneDefinition* GetDefinition() const { return m_Definition; }
		Camera2DComponent* GetMainCamera();
		const Camera2DComponent* GetMainCamera() const;
		EntityHandle GetMainCameraEntity();
		bool SetMainCamera(EntityHandle entity);

		bool IsDirty() const { return m_Dirty; }
		void MarkDirty();
		void ClearDirty() { m_Dirty = false; }

		bool IsUIDirty() const { return m_UIDirty; }
		void MarkUIDirty() { m_UIDirty = true; }
		void ClearUIDirty() { m_UIDirty = false; }

		void MarkTransformDirty(EntityHandle entity);
		std::vector<EntityHandle> ConsumeDirtyTransformEntities();
		bool HasDirtyTransforms() const { return m_TransformHierarchyDirty; }
		void MarkStaticRenderDataDirty();
		uint64_t GetStaticRenderDataVersion() const { return m_StaticRenderDataVersion; }

		// Plays the PlayOnAwake particle/audio of `root` and its descendants. Called after a
		// runtime prefab spawn so freshly created entities awaken the same way scene-start ones
		// do (the one-shot start passes only see entities that exist when play begins).
		void PlayOnAwakeSubtree(EntityHandle root);

		const std::vector<std::string>& GetSceneScriptClassNames() const { return m_SceneScriptClassNames; }
		bool HasSceneScript(const std::string& className) const;
		bool AddSceneScript(const std::string& className);
		bool RemoveSceneScript(size_t index);
		bool MoveSceneScript(size_t fromIndex, size_t toIndex);
		void ClearSceneScripts();
		bool IsSceneScriptEnabled(const std::string& className) const;
		bool SetSceneScriptEnabled(const std::string& className, bool enabled);
		void StartManagedSceneScriptsForPlayMode();

		uint32_t GetSceneScriptHandle(const std::string& className) const;
		void SetSceneScriptFieldValue(const std::string& className, const std::string& fieldName, const std::string& value);
		const std::unordered_map<std::string, std::string>* GetSceneScriptFieldValues(const std::string& className) const;
		const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& GetAllSceneScriptFieldOverrides() const { return m_SceneScriptFieldOverrides; }
		void ClearSceneScriptFieldOverrides(const std::string& className);

		void SetEntityMetaData(
			EntityHandle entity,
			EntityOrigin origin,
			AssetGUID prefabGuid = AssetGUID(0),
			AssetGUID sceneGuid = AssetGUID(0),
			EntityID runtimeId = 0);
		EntityMetaData* GetEntityMetaData(EntityHandle entity);
		const EntityMetaData* GetEntityMetaData(EntityHandle entity) const;
		EntityOrigin GetEntityOrigin(EntityHandle entity) const;
		EntityID GetRuntimeID(EntityHandle entity) const;
		AssetGUID GetSceneEntityGUID(EntityHandle entity) const;
		AssetGUID GetPrefabGUID(EntityHandle entity) const;
		bool TryResolveRuntimeID(EntityID runtimeId, EntityHandle& outHandle) const;
		bool TryResolveSceneGUID(AssetGUID sceneGuid, EntityHandle& outHandle) const;

		// MUST use this for persistent refs — RuntimeID is reallocated on reload.
		uint64_t GetEntityPersistentID(EntityHandle entity) const;

		bool TryResolveEntityRef(uint64_t entityId, EntityHandle& outHandle) const;

		void DeferEntityRefFixup(std::function<void()> fixup);
		void RunPendingEntityRefFixups();

		// Non-zero means the prefab has cross-entity refs; PrefabTemplateCache marks it unbakeable.
		std::size_t GetPendingEntityRefFixupCount() const { return m_PendingEntityRefFixups.size(); }

		UUID GetSceneId() const { return m_SceneId; }
		void SetSceneId(UUID id) { m_SceneId = id; }

	private:
		Scene(const std::string& name, const SceneDefinition* definition, bool IsPersistent);

		void OnTransform2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnTransform2DComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnSpriteRendererComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnSpriteRendererComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnRigidBody2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnRigidBody2DComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnBoxCollider2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnBoxCollider2DComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnCircleCollider2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnCircleCollider2DComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnPolygonCollider2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnPolygonCollider2DComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnAudioSourceComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnScriptComponentDestroy(entt::registry& registry, EntityHandle entity);

		void OnCamera2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnCamera2DComponentDestruct(entt::registry& registry, EntityHandle entity);
		void OnDisabledTagConstruct(entt::registry& registry, EntityHandle entity);
		void OnDisabledTagDestroy(entt::registry& registry, EntityHandle entity);
		void OnStaticTagConstruct(entt::registry& registry, EntityHandle entity);
		void OnStaticTagDestroy(entt::registry& registry, EntityHandle entity);

		void OnParticleSystem2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnParticleSystem2DComponentDestruct(entt::registry& registry, EntityHandle entity);

		void OnFastBody2DConstruct(entt::registry& registry, EntityHandle entity);
		void OnFastBody2DDestroy(entt::registry& registry, EntityHandle entity);
		void OnFastBoxCollider2DConstruct(entt::registry& registry, EntityHandle entity);
		void OnFastBoxCollider2DDestroy(entt::registry& registry, EntityHandle entity);
		void OnFastCircleCollider2DConstruct(entt::registry& registry, EntityHandle entity);
		void OnFastCircleCollider2DDestroy(entt::registry& registry, EntityHandle entity);
		void DestroyEntityInternal(EntityHandle nativeEntity, bool markDirty);
		EntityID AllocateRuntimeEntityID();
		void RegisterEntityIdentity(EntityHandle entity);
		void UnregisterEntityIdentity(EntityHandle entity);
		void TrackEntityDestruction(EntityHandle entity);
		void UntrackEntityDestruction(EntityHandle entity);
		bool IsEntityBeingDestroyed(EntityHandle entity) const;
		void ApplyEntityEnabledState(entt::registry& registry, EntityHandle entity, bool enabled);
		void RefreshMainCameraSelection(entt::registry& registry, EntityHandle preferred = entt::null, EntityHandle excluded = entt::null);

		void RunLifecycleSystems(const std::function<void(ISystem&)>& func, size_t* enteredSystemCount = nullptr);
		void AwakeSystems(size_t* enteredSystemCount = nullptr);
		void StartSystems(size_t* enteredSystemCount = nullptr);
		void UpdateSystems();
		void FixedUpdateSystems();
		void OnPreRenderSystems();
		void DestroyScene();
		void DestroyScene(size_t enabledSystemCount);
		void ForeachEnabledSystem(std::string_view phase, const std::function<void(ISystem&)>& func);

		entt::registry m_Registry;
		std::vector<std::unique_ptr<ISystem>> m_Systems;
		std::vector<std::string> m_SceneScriptClassNames;
		std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_SceneScriptFieldOverrides;
		std::vector<ISystem*> m_AwakenedSystems; // in awakening order; DestroyScene walks in reverse for rollback


		std::string m_Name;
		const SceneDefinition* m_Definition;
		UUID m_SceneId;
		// Access ONLY via GetMainCamera()/GetMainCameraEntity() — both validate the cache.
		// mutable: const overload lazily refreshes without const_cast.
		mutable EntityHandle m_MainCameraEntity = entt::null;

		bool m_IsLoaded = false;
		bool m_Persistent = false;
		bool m_Dirty = false;

		std::size_t m_EntityCount = 0;
		// Guards ClearEntities — prevents RefreshMainCameraSelection from scanning a mutating registry.
		bool m_TearingDown = false;
		bool m_TransformHierarchyDirty = true;
		std::vector<EntityHandle> m_DirtyTransformEntities;
		std::unordered_set<EntityHandle> m_DirtyTransformEntitySet;
		uint64_t m_StaticRenderDataVersion = 1;

		std::vector<std::pair<EntityHandle, float>> m_PendingDestroys;

		std::vector<std::function<void()>> m_PendingEntityRefFixups;
		bool m_UIDirty = true;
		bool m_IsDetached = false;
		EntityID m_NextRuntimeEntityID = 1;
		std::unordered_map<EntityID, EntityHandle> m_RuntimeIdToEntity;
		std::unordered_map<uint64_t, EntityHandle> m_SceneGuidToEntity;
		std::unordered_set<uint32_t> m_EntitiesBeingDestroyed;

		bool m_SuppressConstructHooks = false;

		struct DeferredConstructHook {
			void (Scene::*fn)(entt::registry&, EntityHandle);
			EntityHandle entity;
		};
		std::vector<DeferredConstructHook> m_DeferredConstructHooks;
		// Prevents hooks from re-enqueuing themselves during the flush replay.
		bool m_FlushingDeferredHooks = false;
	};
}
