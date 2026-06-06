#pragma once
#include "Scene/EntityHandle.hpp"
#include "Components/ComponentUtils.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Core/Export.hpp"
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace Index {
	class Scene;
	// Forward-declared to break the cycle Entity.hpp→EntityScriptOps.hpp→Scene.hpp→Entity.hpp;
	// full definition arrives via the EntityScriptOps.hpp include at the bottom of this file.
	class NativeScript;

	// Forward decls needed at parse time; full bodies in EntityScriptOps.hpp (included below the class).
	namespace EntityScriptOps {
		template<typename T>
			requires std::is_base_of_v<NativeScript, T>
		T* AddScriptToEntity(Scene* scene, EntityHandle entity);

		template<typename T>
			requires std::is_base_of_v<NativeScript, T>
		T* GetScriptOnEntity(Scene* scene, EntityHandle entity);

		template<typename T>
			requires std::is_base_of_v<NativeScript, T>
		bool HasScriptOnEntity(Scene* scene, EntityHandle entity);

		template<typename T>
			requires std::is_base_of_v<NativeScript, T>
		bool TryGetScriptOnEntity(Scene* scene, EntityHandle entity, T*& out);

		template<typename T>
			requires std::is_base_of_v<NativeScript, T>
		bool RemoveScriptFromEntity(Scene* scene, EntityHandle entity);
	}

	class INDEX_API Entity {
		friend class Scene;
		friend class EntityHelper;

	public:
		static Entity Create();
		static Entity Create(const std::string& name);

		static Entity Instantiate(Entity source);

		static void Destroy(Entity entity);
		static void Destroy(Entity entity, float delay);

		template<typename... TComponent>
		bool HasComponents() const {
			if (!IsValid()) return false;
			return (HasComponent<TComponent>() && ...);
		}

		template<typename... TComponent>
		static Entity CreateWith(const std::string& name = "") {
			Entity e = name.empty() ? Create() : Create(name);
			if (!e.IsValid()) return e;
			(e.AddComponent<TComponent>(), ...);
			return e;
		}

		void Destroy();
		static const Entity Null;

		// IsValid()==false; only carries Scene* for callers (e.g. PropertyDrawer) that need
		// to dispatch scene-scoped logic through an Entity API but have no real handle.
		static Entity MakeScenePlaceholder(Scene& scene);


		// ── Component branch (ECS data — Transform2D, SpriteRenderer, …) ───
		template<typename TComponent, typename... Args>
			requires (!std::is_empty_v<TComponent> && !std::is_base_of_v<NativeScript, TComponent>)
		TComponent& AddComponent(Args&&... args) {
			EnsureValid("Cannot add component to invalid entity");
			return  ComponentUtils::AddComponent<TComponent>(*m_Registry, m_EntityHandle, std::forward<Args>(args)...);
		}

		template<typename TTag>
			requires (std::is_empty_v<TTag> && !std::is_base_of_v<NativeScript, TTag>)
		void AddComponent() {
			EnsureValid("Cannot add tag component to invalid entity");
			ComponentUtils::AddComponent<TTag>(*m_Registry, m_EntityHandle);
		}

		template<typename TComponent>
			requires (!std::is_base_of_v<NativeScript, TComponent>)
		bool HasComponent() const {
			if (!IsValid()) {
				return false;
			}
			return  ComponentUtils::HasComponent<TComponent>(*m_Registry, m_EntityHandle);
		}

		template<typename... TComponent>
		bool HasAnyComponent() const {
			if (!IsValid()) {
				return false;
			}
			return  ComponentUtils::HasAnyComponent<TComponent...>(*m_Registry, m_EntityHandle);
		}

		template<typename TComponent>
			requires (!std::is_base_of_v<NativeScript, TComponent>)
		TComponent& GetComponent() {
			EnsureValid("Cannot get component from invalid entity");
			return  ComponentUtils::GetComponent<TComponent>(*m_Registry, m_EntityHandle);
		}

		template<typename TComponent>
			requires (!std::is_base_of_v<NativeScript, TComponent>)
		const TComponent& GetComponent() const {
			EnsureValid("Cannot get component from invalid entity");
			return  ComponentUtils::GetComponent<TComponent>(*m_Registry, m_EntityHandle);
		}

		template<typename TComponent>
			requires (!std::is_base_of_v<NativeScript, TComponent>)
		bool TryGetComponent(TComponent*& out) {
			if (!IsValid()) {
				out = nullptr;
				return false;
			}
			out = ComponentUtils::TryGetComponent<TComponent>(*m_Registry, m_EntityHandle);
			return out != nullptr;
		}

		template<typename TComponent>
			requires (!std::is_base_of_v<NativeScript, TComponent>)
		void RemoveComponent() {
			if (!IsValid()) {
				return;
			}
			ComponentUtils::RemoveComponent<TComponent>(*m_Registry, m_EntityHandle);
		}

		// ── Script branch (NativeScript-derived) — returns T* (nullable); definitions in EntityScriptOps.hpp ───
		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		TScript* AddComponent();

		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		bool HasComponent() const;

		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		TScript* GetComponent();

		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		const TScript* GetComponent() const;

		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		bool TryGetComponent(TScript*& out);

		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		void RemoveComponent();

		std::string GetName() const;
		const EntityMetaData* GetMetaData() const;
		EntityOrigin GetOrigin() const;
		EntityID GetRuntimeID() const;
		AssetGUID GetSceneGUID() const;
		AssetGUID GetPrefabGUID() const;
		bool IsSceneEntity() const;
		bool IsPrefabInstance() const;
		bool IsRuntime() const;

		EntityHandle GetHandle() const;
		Scene* GetScene() { return m_Scene; }
		const Scene* GetScene() const { return m_Scene; }
		bool IsValid() const { return m_Registry && m_EntityHandle != entt::null && m_Registry->valid(m_EntityHandle); }

		void SetStatic(bool isStatic);
		void SetEnabled(bool enabled);

		// ── Parent-child hierarchy ────────────────────────────────────
		// Pass Entity::Null to detach. Silently refuses cycles (no-op) so a buggy drag-drop can't corrupt the graph.
		void SetParent(Entity parent);

		// Returns Entity::Null when this is a root.
		Entity GetParent() const;

		std::span<const EntityHandle> GetChildren() const;

		bool HasParent() const;
		bool IsAncestorOf(Entity other) const;

	private:
		void EnsureValid(const char* message) const;

		explicit Entity(EntityHandle e, Scene& scene);
		explicit Entity(EntityHandle e, Scene* scene);
		EntityHandle    m_EntityHandle;
		entt::registry* m_Registry;
		Scene*          m_Scene;
	};

	inline bool operator==(const Entity& a, const Entity& b) { return a.GetHandle() == b.GetHandle(); }
	inline bool operator!=(const Entity& a, const Entity& b) { return !(a == b); }
}

#include "Scripting/EntityScriptOps.hpp"

namespace Index {

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	TScript* Entity::AddComponent() {
		if (!IsValid()) return nullptr;
		return EntityScriptOps::AddScriptToEntity<TScript>(m_Scene, m_EntityHandle);
	}

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	bool Entity::HasComponent() const {
		if (!IsValid()) return false;
		return EntityScriptOps::HasScriptOnEntity<TScript>(m_Scene, m_EntityHandle);
	}

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	TScript* Entity::GetComponent() {
		if (!IsValid()) return nullptr;
		return EntityScriptOps::GetScriptOnEntity<TScript>(m_Scene, m_EntityHandle);
	}

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	const TScript* Entity::GetComponent() const {
		if (!IsValid()) return nullptr;
		return EntityScriptOps::GetScriptOnEntity<TScript>(m_Scene, m_EntityHandle);
	}

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	bool Entity::TryGetComponent(TScript*& out) {
		if (!IsValid()) { out = nullptr; return false; }
		return EntityScriptOps::TryGetScriptOnEntity<TScript>(m_Scene, m_EntityHandle, out);
	}

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	void Entity::RemoveComponent() {
		if (!IsValid()) return;
		(void)EntityScriptOps::RemoveScriptFromEntity<TScript>(m_Scene, m_EntityHandle);
	}

}
