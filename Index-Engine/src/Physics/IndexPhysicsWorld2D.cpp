#include "pch.hpp"
#include "Physics/IndexPhysicsWorld2D.hpp"

#include <BoxCollider.hpp>
#include <CircleCollider.hpp>

namespace Index {

	IndexPhysicsWorld2D::IndexPhysicsWorld2D() : m_World() {}

	IndexPhysicsWorld2D::IndexPhysicsWorld2D(const AxiomPhys::WorldSettings& settings)
		: m_World(settings) {
	}

	void IndexPhysicsWorld2D::Step(float dt) {
		m_World.Step(dt);
		DispatchContacts();
	}

	void IndexPhysicsWorld2D::Destroy() {
		m_ContactCallbacks.clear();
		m_BodyToEntity.clear();
		m_BodyToScene.clear();
		m_AttachedColliderKind.clear();
		m_PreviousContacts.clear();

		for (auto& [key, body] : m_Bodies) {
			m_World.DetachCollider(*body);
			m_World.UnregisterBody(*body);
		}
		for (auto& [key, collider] : m_Colliders) {
			(void)key;
			m_World.UnregisterCollider(*collider);
		}

		m_Bodies.clear();
		m_Colliders.clear();
	}

	AxiomPhys::Body* IndexPhysicsWorld2D::CreateBody(EntityHandle entity, AxiomPhys::BodyType type, Scene* scene) {
		uint32_t key = static_cast<uint32_t>(entity);
		if (m_Bodies.count(key)) {
			IDX_CORE_WARN_TAG("IndexPhysics", "Entity already has a Axiom-Physics body");
			return m_Bodies[key].get();
		}

		auto body = std::make_unique<AxiomPhys::Body>(type);
		AxiomPhys::Body* ptr = body.get();
		m_World.RegisterBody(*ptr);
		m_BodyToEntity[ptr] = entity;
		m_BodyToScene[ptr] = scene;
		m_Bodies[key] = std::move(body);
		return ptr;
	}

	void IndexPhysicsWorld2D::DestroyBody(EntityHandle entity) {
		uint32_t key = static_cast<uint32_t>(entity);

		auto it = m_Bodies.find(key);
		if (it == m_Bodies.end()) return;

		AxiomPhys::Body* ptr = it->second.get();

		// Detach (but do NOT destroy) any attached collider. Collider
		// components own their own lifetime and clean up via their own
		// on_destroy hook; destroying their AxiomPhys::Collider here
		// would dangle the raw m_Collider pointer stored in the still-
		// alive component. After detach, the surviving collider is
		// registered-but-orphaned in the world until its component goes.
		m_World.DetachCollider(*ptr);
		m_AttachedColliderKind.erase(key);

		m_World.UnregisterBody(*ptr);
		m_BodyToEntity.erase(ptr);
		m_BodyToScene.erase(ptr);
		m_Bodies.erase(it);
		m_ContactCallbacks.erase(key);

		// Drop any cached contact pair referencing this entity so the next
		// DispatchScriptContacts pass doesn't synthesise a phantom Exit for
		// an entity that no longer exists.
		for (auto pairIt = m_PreviousContacts.begin(); pairIt != m_PreviousContacts.end(); ) {
			if (pairIt->entityA == key || pairIt->entityB == key) {
				pairIt = m_PreviousContacts.erase(pairIt);
			} else {
				++pairIt;
			}
		}
	}

	AxiomPhys::Body* IndexPhysicsWorld2D::GetBody(EntityHandle entity) {
		uint32_t key = static_cast<uint32_t>(entity);
		auto it = m_Bodies.find(key);
		return it != m_Bodies.end() ? it->second.get() : nullptr;
	}

	AxiomPhys::BoxCollider* IndexPhysicsWorld2D::CreateBoxCollider(EntityHandle entity, const Vec2& halfExtents) {
		uint32_t key = static_cast<uint32_t>(entity);

		// Replace only the BOX collider — keeps a coexisting circle collider's pointer valid.
		DestroyCollider(entity, FastColliderKind::Box);

		auto collider = std::make_unique<AxiomPhys::BoxCollider>(AxiomPhys::Vec2(halfExtents.x, halfExtents.y));
		AxiomPhys::BoxCollider* ptr = collider.get();
		m_World.RegisterCollider(*ptr);

		// Only attach to the body if nothing else is currently attached.
		// Inspector blocks user-driven dual colliders via DeclareConflict,
		// but deserialization and scripting can still produce both kinds
		// on the same entity. In that case the second creation just
		// registers in the world without attaching, so the previously
		// attached collider keeps its body-side state.
		auto bodyIt = m_Bodies.find(key);
		if (bodyIt != m_Bodies.end()
			&& m_AttachedColliderKind.find(key) == m_AttachedColliderKind.end()) {
			m_World.AttachCollider(*bodyIt->second, *ptr);
			m_AttachedColliderKind[key] = FastColliderKind::Box;
		}

		m_Colliders[ColliderKey{ key, FastColliderKind::Box }] = std::move(collider);
		return ptr;
	}

	AxiomPhys::CircleCollider* IndexPhysicsWorld2D::CreateCircleCollider(EntityHandle entity, float radius) {
		uint32_t key = static_cast<uint32_t>(entity);

		DestroyCollider(entity, FastColliderKind::Circle);

		auto collider = std::make_unique<AxiomPhys::CircleCollider>(radius);
		AxiomPhys::CircleCollider* ptr = collider.get();
		m_World.RegisterCollider(*ptr);

		auto bodyIt = m_Bodies.find(key);
		if (bodyIt != m_Bodies.end()
			&& m_AttachedColliderKind.find(key) == m_AttachedColliderKind.end()) {
			m_World.AttachCollider(*bodyIt->second, *ptr);
			m_AttachedColliderKind[key] = FastColliderKind::Circle;
		}

		m_Colliders[ColliderKey{ key, FastColliderKind::Circle }] = std::move(collider);
		return ptr;
	}

	void IndexPhysicsWorld2D::DestroyCollider(EntityHandle entity, FastColliderKind kind) {
		const uint32_t key = static_cast<uint32_t>(entity);
		auto it = m_Colliders.find(ColliderKey{ key, kind });
		if (it == m_Colliders.end()) return;

		AxiomPhys::Collider* ptr = it->second.get();

		// Only detach if THIS kind is the one currently attached to the
		// body. Detaching when a different kind is attached would silently
		// strip the wrong collider from the body's collision state.
		auto bodyIt = m_Bodies.find(key);
		auto attachedIt = m_AttachedColliderKind.find(key);
		const bool wasAttached =
			bodyIt != m_Bodies.end()
			&& attachedIt != m_AttachedColliderKind.end()
			&& attachedIt->second == kind;
		if (wasAttached) {
			m_World.DetachCollider(*bodyIt->second);
			m_AttachedColliderKind.erase(attachedIt);
		}

		m_World.UnregisterCollider(*ptr);
		m_Colliders.erase(it);

		// If a different-kind collider still exists on this entity (the
		// dual-collider case made possible by deserialization), promote
		// it to the active attachment so the body keeps colliding.
		if (wasAttached && bodyIt != m_Bodies.end()) {
			const FastColliderKind otherKind =
				kind == FastColliderKind::Box ? FastColliderKind::Circle : FastColliderKind::Box;
			auto otherIt = m_Colliders.find(ColliderKey{ key, otherKind });
			if (otherIt != m_Colliders.end()) {
				m_World.AttachCollider(*bodyIt->second, *otherIt->second);
				m_AttachedColliderKind[key] = otherKind;
			}
		}
	}

	void IndexPhysicsWorld2D::PurgeBodiesForScene(Scene* scene) {
		if (!scene) return;

		// Collect first — DestroyBody mutates m_BodyToScene / m_BodyToEntity, so
		// we must not iterate those maps while destroying their entries.
		std::vector<EntityHandle> entities;
		for (const auto& [body, ownerScene] : m_BodyToScene) {
			if (ownerScene == scene) {
				auto it = m_BodyToEntity.find(body);
				if (it != m_BodyToEntity.end()) entities.push_back(it->second);
			}
		}

		// DestroyAllCollidersOnEntity wipes the (entity, kind) collider entries;
		// DestroyBody erases the body plus its m_Bodies / m_BodyToEntity /
		// m_BodyToScene / m_ContactCallbacks entries and drops cached contact
		// pairs referencing the entity. After this loop nothing tied to the
		// unloaded scene — least of all a dangling Scene* — remains.
		for (EntityHandle e : entities) {
			DestroyAllCollidersOnEntity(e);
			DestroyBody(e);
		}
	}

	void IndexPhysicsWorld2D::DestroyAllCollidersOnEntity(EntityHandle entity) {
		const uint32_t key = static_cast<uint32_t>(entity);
		auto bodyIt = m_Bodies.find(key);
		if (bodyIt != m_Bodies.end()) {
			m_World.DetachCollider(*bodyIt->second);
		}
		m_AttachedColliderKind.erase(key);
		for (auto it = m_Colliders.begin(); it != m_Colliders.end(); ) {
			if (it->first.entity == key) {
				m_World.UnregisterCollider(*it->second);
				it = m_Colliders.erase(it);
			} else {
				++it;
			}
		}
	}

	void IndexPhysicsWorld2D::RegisterContactCallback(EntityHandle entity, IndexContactCallback callback) {
		m_ContactCallbacks[static_cast<uint32_t>(entity)] = std::move(callback);
	}

	void IndexPhysicsWorld2D::UnregisterContactCallback(EntityHandle entity) {
		m_ContactCallbacks.erase(static_cast<uint32_t>(entity));
	}

	void IndexPhysicsWorld2D::DispatchScriptContacts(
		const ScriptDispatchCallback& onEnter,
		const ScriptDispatchCallback& onStay,
		const ScriptDispatchCallback& onExit)
	{
		// AxiomPhys reports the CURRENT overlap set per frame. Derive
		// Enter / Stay / Exit by diffing against m_PreviousContacts (the
		// same trick CollisionDispatcher uses on top of Box2D's events).
		std::unordered_set<ContactPair, ContactPairHash> currentContacts;
		struct ResolvedContact {
			ContactPair Pair;
			Collision2D Collision;
		};
		std::vector<ResolvedContact> currentResolved;

		const auto& contacts = m_World.GetContacts();
		currentContacts.reserve(contacts.size());
		currentResolved.reserve(contacts.size());

		for (const auto& contact : contacts) {
			auto itA = m_BodyToEntity.find(contact.bodyA);
			auto itB = m_BodyToEntity.find(contact.bodyB);
			if (itA == m_BodyToEntity.end() || itB == m_BodyToEntity.end()) continue;

			Scene* sceneA = nullptr;
			Scene* sceneB = nullptr;
			if (auto sit = m_BodyToScene.find(contact.bodyA); sit != m_BodyToScene.end()) sceneA = sit->second;
			if (auto sit = m_BodyToScene.find(contact.bodyB); sit != m_BodyToScene.end()) sceneB = sit->second;

			uint32_t a = static_cast<uint32_t>(itA->second);
			uint32_t b = static_cast<uint32_t>(itB->second);
			if (a > b) {
				std::swap(a, b);
				std::swap(sceneA, sceneB);
			}
			ContactPair pair{ a, b };
			if (!currentContacts.insert(pair).second) {
				continue;
			}

			// contact.normal points from A→B in the original (pre-swap) order;
			// after the canonical-pair swap above, the "A" side may not match
			// AxiomPhys's A. Use the midpoint between body positions as the
			// contact point — same approximation Box2D's path falls back to
			// when an explicit point isn't available.
			Vec2 midpoint{ 0.0f, 0.0f };
			if (contact.bodyA && contact.bodyB) {
				auto pa = contact.bodyA->GetPosition();
				auto pb = contact.bodyB->GetPosition();
				midpoint = { (pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f };
			}

			Collision2D collision{};
			collision.entityA = static_cast<EntityHandle>(static_cast<entt::entity>(pair.entityA));
			collision.entityB = static_cast<EntityHandle>(static_cast<entt::entity>(pair.entityB));
			collision.sceneA = sceneA;
			collision.sceneB = sceneB;
			collision.contactPoint = midpoint;

			currentResolved.push_back({ pair, collision });
		}

		// Enter + Stay
		for (const auto& resolved : currentResolved) {
			if (m_PreviousContacts.count(resolved.Pair)) {
				if (onStay) onStay(resolved.Collision);
			} else {
				if (onEnter) onEnter(resolved.Collision);
			}
		}

		// Exit — pairs that were active last frame but aren't anymore.
		if (onExit) {
			for (const ContactPair& pair : m_PreviousContacts) {
				if (currentContacts.count(pair)) continue;
				Collision2D collision{};
				collision.entityA = static_cast<EntityHandle>(static_cast<entt::entity>(pair.entityA));
				collision.entityB = static_cast<EntityHandle>(static_cast<entt::entity>(pair.entityB));
				// Scene context is best-effort — the bodies may already be torn
				// down. Look up via the still-live m_BodyToScene map by entity.
				for (const auto& [body, ent] : m_BodyToEntity) {
					if (static_cast<uint32_t>(ent) == pair.entityA) {
						auto sit = m_BodyToScene.find(body);
						if (sit != m_BodyToScene.end()) collision.sceneA = sit->second;
					}
					else if (static_cast<uint32_t>(ent) == pair.entityB) {
						auto sit = m_BodyToScene.find(body);
						if (sit != m_BodyToScene.end()) collision.sceneB = sit->second;
					}
				}
				onExit(collision);
			}
		}

		m_PreviousContacts = std::move(currentContacts);
	}

	void IndexPhysicsWorld2D::DispatchContacts() {
		const auto& contacts = m_World.GetContacts();
		for (const auto& contact : contacts) {
			EntityHandle entityA = entt::null;
			EntityHandle entityB = entt::null;

			auto itA = m_BodyToEntity.find(contact.bodyA);
			auto itB = m_BodyToEntity.find(contact.bodyB);
			if (itA != m_BodyToEntity.end()) entityA = itA->second;
			if (itB != m_BodyToEntity.end()) entityB = itB->second;

			IndexContact2D indexContact{
				entityA, entityB,
				{ contact.normal.x, contact.normal.y },
				contact.penetration
			};

			// Copy the std::function objects out of the map BEFORE invoking. A callback
			// can call UnregisterContactCallback (or destroy its own entity, which routes
			// here too); erasing the map entry mid-call would destroy the std::function
			// while its operator() is still on the stack. CollisionDispatcher::DispatchSafe
			// uses the same pattern.
			IndexContactCallback cbA;
			IndexContactCallback cbB;
			if (entityA != entt::null) {
				auto cbIt = m_ContactCallbacks.find(static_cast<uint32_t>(entityA));
				if (cbIt != m_ContactCallbacks.end()) cbA = cbIt->second;
			}
			if (entityB != entt::null) {
				auto cbIt = m_ContactCallbacks.find(static_cast<uint32_t>(entityB));
				if (cbIt != m_ContactCallbacks.end()) cbB = cbIt->second;
			}

			if (cbA) cbA(indexContact);
			if (cbB) cbB(indexContact);
		}
	}

}
