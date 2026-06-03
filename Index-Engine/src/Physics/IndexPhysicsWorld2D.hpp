#pragma once
#include "Collections/Vec2.hpp"
#include "Core/Export.hpp"
#include "Physics/Collision2D.hpp"
#include "Physics/IndexContact2D.hpp"
#include "Physics/IndexPhysicsInterop.hpp"
#include "Scene/EntityHandle.hpp"

#include <PhysicsWorld.hpp>
#include <WorldSettings.hpp>
#include <Contact.hpp>
#include <BoxCollider.hpp>
#include <CircleCollider.hpp>

#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <vector>

namespace Index {
	using IndexContactCallback = std::function<void(const IndexContact2D&)>;

	/// Engine-level wrapper around the Index-Physics PhysicsWorld. Not INDEX_API: owns unique_ptr map members (MSVC dllexport would instantiate copy-op).
	class IndexPhysicsWorld2D {
	public:
		IndexPhysicsWorld2D();
		explicit IndexPhysicsWorld2D(const IndexPhys::WorldSettings& settings);

		void Step(float dt);
		void Destroy();

		// Called by PhysicsSystem2D::FixedUpdate after Step so Fast* collision events match Box2D collider timing.
		using ScriptDispatchCallback = std::function<void(const Collision2D&)>;
		void DispatchScriptContacts(
			const ScriptDispatchCallback& onEnter,
			const ScriptDispatchCallback& onStay,
			const ScriptDispatchCallback& onExit);

		IndexPhys::PhysicsWorld& GetWorld() { return m_World; }
		const IndexPhys::PhysicsWorld& GetWorld() const { return m_World; }

		void SetSettings(const IndexPhys::WorldSettings& settings) { m_World.SetSettings(settings); }
		const IndexPhys::WorldSettings& GetSettings() const { return m_World.GetSettings(); }

		// `scene` routes collision events to the right ScriptSystem; omit for the SceneManager::GetActiveScene fallback.
		IndexPhys::Body* CreateBody(EntityHandle entity, IndexPhys::BodyType type, Scene* scene = nullptr);
		void DestroyBody(EntityHandle entity);
		IndexPhys::Body* GetBody(EntityHandle entity);

		// Colliders keyed by (entity, kind): programmatic dual-collider adds no longer dangle the existing pointer.
		enum class FastColliderKind : uint8_t {
			Box,
			Circle,
		};

		IndexPhys::BoxCollider* CreateBoxCollider(EntityHandle entity, const Vec2& halfExtents);
		IndexPhys::CircleCollider* CreateCircleCollider(EntityHandle entity, float radius);
		void DestroyCollider(EntityHandle entity, FastColliderKind kind);
		// DestroyBody intentionally does NOT call this: collider components own their lifetime and DestroyAllCollidersOnEntity would dangle their raw m_Collider pointers.
		void DestroyAllCollidersOnEntity(EntityHandle entity);

		// Called after ClearEntities to prevent a dangling Scene* in m_BodyToScene (read every frame by DispatchScriptContacts).
		void PurgeBodiesForScene(Scene* scene);

		// Contact callbacks per entity
		void RegisterContactCallback(EntityHandle entity, IndexContactCallback callback);
		void UnregisterContactCallback(EntityHandle entity);

		size_t GetBodyCount() const { return m_World.GetBodyCount(); }

		// Map a query-hit collider back to its owning (scene, entity) via its attached
		// body. Returns false when the collider has no registered body (e.g. an
		// unattached dual collider), leaving the out-params at scene=null / entity=null.
		bool ResolveColliderEntity(const IndexPhys::Collider* collider, Scene*& outScene, EntityHandle& outEntity) const;

	private:
		void DispatchContacts();

		IndexPhys::PhysicsWorld m_World;

		struct ColliderKey {
			uint32_t entity;
			FastColliderKind kind;
			bool operator==(const ColliderKey& o) const noexcept {
				return entity == o.entity && kind == o.kind;
			}
		};
		struct ColliderKeyHash {
			size_t operator()(const ColliderKey& k) const noexcept {
				const uint64_t packed =
					(static_cast<uint64_t>(k.entity) << 8) |
					static_cast<uint64_t>(static_cast<uint8_t>(k.kind));
				return std::hash<uint64_t>{}(packed);
			}
		};

		// Ownership: bodies keyed by entity, colliders keyed by (entity, kind)
		std::unordered_map<uint32_t, std::unique_ptr<IndexPhys::Body>> m_Bodies;
		std::unordered_map<ColliderKey, std::unique_ptr<IndexPhys::Collider>, ColliderKeyHash> m_Colliders;

		// Tracks which kind is currently attached to the body (only one can be live); without this DestroyCollider would detach the wrong collider.
		std::unordered_map<uint32_t, FastColliderKind> m_AttachedColliderKind;

		std::unordered_map<IndexPhys::Body*, EntityHandle> m_BodyToEntity;
		std::unordered_map<IndexPhys::Body*, Scene*> m_BodyToScene;

		// Contact callbacks per entity
		std::unordered_map<uint32_t, IndexContactCallback> m_ContactCallbacks;

		// Last-frame contact pairs for Enter/Stay/Exit derivation.
		struct ContactPair {
			uint32_t entityA = 0;
			uint32_t entityB = 0;
			bool operator==(const ContactPair& other) const noexcept {
				return entityA == other.entityA && entityB == other.entityB;
			}
		};
		struct ContactPairHash {
			size_t operator()(const ContactPair& key) const noexcept {
				return std::hash<uint64_t>{}(
					(static_cast<uint64_t>(key.entityA) << 32) |
					static_cast<uint64_t>(key.entityB));
			}
		};
		std::unordered_set<ContactPair, ContactPairHash> m_PreviousContacts;
	};

}
