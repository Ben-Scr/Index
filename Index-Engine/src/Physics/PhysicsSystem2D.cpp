#include "pch.hpp"

#include <Components/Physics/Rigidbody2DComponent.hpp>
#include <Components/Physics/FastBody2DComponent.hpp>
#include <Components/Physics/BoxCollider2DComponent.hpp>
#include <Components/Physics/CircleCollider2DComponent.hpp>
#include <Components/Physics/PolygonCollider2DComponent.hpp>
#include <Components/Physics/FastBoxCollider2DComponent.hpp>
#include <Components/Physics/FastCircleCollider2DComponent.hpp>
#include <Components/General/Transform2DComponent.hpp>
#include <Components/General/HierarchyComponent.hpp>
#include <Components/Tags.hpp>

#include "Physics/PhysicsSystem2D.hpp"
#include "Physics/Box2DWorld.hpp"
#include "Physics/Collision2D.hpp"

#include "Core/Application.hpp"
#include "Profiling/Profiler.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/ScriptSystem.hpp"


namespace Index {
	bool PhysicsSystem2D::s_IsEnabled = true;
	std::optional<Box2DWorld> PhysicsSystem2D::s_MainWorld;
	std::optional<IndexPhysicsWorld2D> PhysicsSystem2D::s_IndexWorld;

	namespace {
		// Refresh tf.Position/Rotation/Scale from LocalPosition/LocalRotation/LocalScale
		// for a body-owned entity. TransformHierarchySystem deliberately skips body-owned
		// entities (physics is authoritative for their world transform in play mode), so
		// without this step a user editing Position via the inspector — which writes only
		// LocalPosition through Transform2DComponent::SetPosition — would leave tf.Position
		// stale; rb.SetTransform(tf) below would then push the body's own stale position
		// right back to the body, silently discarding the edit. Mirrors the composition
		// the C# Transform2D_SetLocalPosition binding (ScriptBindings.cpp) already does.
		void ComposeBodyOwnedWorldFromLocal(entt::registry& registry, EntityHandle entity, Transform2DComponent& tf) {
			const HierarchyComponent* hierarchy = registry.try_get<HierarchyComponent>(entity);
			const EntityHandle parent = hierarchy ? hierarchy->Parent : entt::null;
			const Transform2DComponent* parentTf = (parent != entt::null)
				? registry.try_get<Transform2DComponent>(parent) : nullptr;
			if (!parentTf) {
				tf.Position = tf.LocalPosition;
				tf.Rotation = tf.LocalRotation;
				tf.Scale = tf.LocalScale;
				return;
			}
			tf.Position = parentTf->TransformPoint(tf.LocalPosition);
			tf.Rotation = parentTf->Rotation + tf.LocalRotation;
			tf.Scale = { parentTf->Scale.x * tf.LocalScale.x,
						 parentTf->Scale.y * tf.LocalScale.y };
		}

		// Inverse of ComposeBodyOwnedWorldFromLocal — derive Local* from the
		// physics-driven world transform after a step so a later inspector
		// read or ComposeBodyOwnedWorldFromLocal call sees the new authored
		// value instead of the stale pre-step one. Without this, the
		// LocalPosition / LocalRotation / LocalScale fields drift away from
		// the visible world transform whenever the body is moved by physics,
		// which breaks the "Transform2D follows the body and vice versa"
		// invariant the inspector and user scripts rely on.
		void DecomposeBodyOwnedWorldToLocal(entt::registry& registry, EntityHandle entity, Transform2DComponent& tf) {
			const HierarchyComponent* hierarchy = registry.try_get<HierarchyComponent>(entity);
			const EntityHandle parent = hierarchy ? hierarchy->Parent : entt::null;
			const Transform2DComponent* parentTf = (parent != entt::null)
				? registry.try_get<Transform2DComponent>(parent) : nullptr;
			if (!parentTf) {
				tf.LocalPosition = tf.Position;
				tf.LocalRotation = tf.Rotation;
				tf.LocalScale = tf.Scale;
				return;
			}
			const Vec2 translated{ tf.Position.x - parentTf->Position.x,
								   tf.Position.y - parentTf->Position.y };
			const Vec2 unrotated = Rotate(translated, -parentTf->Rotation);
			tf.LocalPosition = {
				std::abs(parentTf->Scale.x) > 0.00001f ? unrotated.x / parentTf->Scale.x : unrotated.x,
				std::abs(parentTf->Scale.y) > 0.00001f ? unrotated.y / parentTf->Scale.y : unrotated.y };
			tf.LocalRotation = tf.Rotation - parentTf->Rotation;
			tf.LocalScale = {
				std::abs(parentTf->Scale.x) > 0.00001f ? tf.Scale.x / parentTf->Scale.x : tf.Scale.x,
				std::abs(parentTf->Scale.y) > 0.00001f ? tf.Scale.y / parentTf->Scale.y : tf.Scale.y };
		}
	}

	void PhysicsSystem2D::Initialize() {
		s_MainWorld.emplace();
		s_IndexWorld.emplace();
		IDX_CORE_INFO_TAG("PhysicsSystem", "Box2D + Axiom-Physics initialized");
	}

	void PhysicsSystem2D::SyncTransformsToPhysics() {
		// Pre-step sync. Collider shape rebuild runs FIRST so the dirty-read
		// happens before the body loops clear the dirty flag — otherwise a
		// rigidbody's body-sync would race the polygon/circle rebuild and the
		// collider would lag a frame behind the transform.
		SceneManager::Get().ForeachLoadedScene([](Scene& scene) {
			auto& registry = scene.GetRegistry();

			// Refresh World* from Local* for body-owned entities BEFORE the
			// collider / body sync reads tf.Position and tf.Scale.
			// TransformHierarchySystem skips body-owned entities, so without
			// this pass an inspector edit (which writes only LocalPosition /
			// LocalScale via Transform2DComponent::SetPosition / SetScale)
			// would leave the world snapshot stale, and the loops below would
			// either short-circuit (collider scale unchanged) or push the
			// body's own stale position right back into the body.
			for (auto [ent, tf] : registry.view<Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (!tf.IsDirty()) continue;
				if (!registry.any_of<Rigidbody2DComponent, FastBody2DComponent>(ent)) continue;
				ComposeBodyOwnedWorldFromLocal(registry, ent, tf);
			}

			for (auto [ent, box, tf] : registry.view<BoxCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (tf.IsDirty() && box.IsValid()) {
					box.SyncWithTransform(scene);
				}
			}

			for (auto [ent, circle, tf] : registry.view<CircleCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (tf.IsDirty() && circle.IsValid()) {
					circle.SyncWithTransform(scene);
				}
			}

			for (auto [ent, poly, tf] : registry.view<PolygonCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (tf.IsDirty() && poly.IsValid()) {
					poly.SyncWithTransform(scene);
				}
			}

			// Mirror the Box2D collider scale-sync for AxiomPhys colliders so
			// the Fast* variants resize when the Transform2D scale changes,
			// matching the standard collider behaviour the user expects.
			for (auto [ent, box, tf] : registry.view<FastBoxCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (tf.IsDirty() && box.IsValid()) {
					box.SyncWithTransform(scene);
				}
			}

			for (auto [ent, circle, tf] : registry.view<FastCircleCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (tf.IsDirty() && circle.IsValid()) {
					circle.SyncWithTransform(scene);
				}
			}

			for (auto [ent, rb, tf] : registry.view<Rigidbody2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (tf.IsDirty() && rb.IsValid()) {
					rb.SetTransform(tf);
					tf.ClearDirty();
				}
			}

			for (auto [ent, body, tf] : registry.view<FastBody2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (tf.IsDirty() && body.m_Body) {
					body.SetPosition(tf.Position);
					tf.ClearDirty();
				}
			}

			// Clear dirty for collider-only entities (no body sync touched them).
			for (auto [ent, box, tf] : registry.view<BoxCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag, Rigidbody2DComponent, FastBody2DComponent>).each()) {
				if (tf.IsDirty()) {
					tf.ClearDirty();
				}
			}
			for (auto [ent, circle, tf] : registry.view<CircleCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag, Rigidbody2DComponent, FastBody2DComponent>).each()) {
				if (tf.IsDirty()) {
					tf.ClearDirty();
				}
			}
			for (auto [ent, poly, tf] : registry.view<PolygonCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag, Rigidbody2DComponent, FastBody2DComponent>).each()) {
				if (tf.IsDirty()) {
					tf.ClearDirty();
				}
			}
			// Fast* collider-only entities follow the same pattern — clear dirty
			// after the size sync above has run.
			for (auto [ent, box, tf] : registry.view<FastBoxCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag, Rigidbody2DComponent, FastBody2DComponent>).each()) {
				if (tf.IsDirty()) {
					tf.ClearDirty();
				}
			}
			for (auto [ent, circle, tf] : registry.view<FastCircleCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag, Rigidbody2DComponent, FastBody2DComponent>).each()) {
				if (tf.IsDirty()) {
					tf.ClearDirty();
				}
			}
		});
	}

	void PhysicsSystem2D::Update() {
		if (!s_IsEnabled) return;
		// FixedUpdate already runs the same per-scene sync during play mode, so
		// repeating it from Update doubles the dirty-flag walk every frame for
		// no gain. Editor mode still needs Update to drive the sync because
		// FixedUpdate is gated off.
		if (Application::GetIsPlaying() && !Application::IsPaused()) {
			return;
		}
		SyncTransformsToPhysics();
	}

	/* static */ void PhysicsSystem2D::WakeAllBodies() {
		// AxiomPhys (FastBody2D) has no sleep optimization, so only Box2D
		// rigidbodies need their sleep timer reset.
		SceneManager::Get().ForeachLoadedScene([](Scene& scene) {
			auto& registry = scene.GetRegistry();
			for (auto [ent, rb] : registry.view<Rigidbody2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (rb.IsValid()) {
					b2Body_SetAwake(rb.GetBodyHandle(), true);
				}
			}
		});
	}

	void PhysicsSystem2D::FixedUpdate(float dt) {
		INDEX_PROFILE_SCOPE("Physics");
		if (!s_IsEnabled) return;
		if (!s_MainWorld || !s_IndexWorld) return;

		SyncTransformsToPhysics();

		// Step both worlds, then sync poses back, then dispatch callbacks
		// — so callbacks see fully-settled scene state.
		s_MainWorld->Step(dt);
		s_IndexWorld->Step(dt);

		SceneManager::Get().ForeachLoadedScene([](Scene& scene) {
			auto& registry = scene.GetRegistry();

			for (auto [ent, rb, tf] : registry.view<Rigidbody2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (!rb.IsValid()) continue;
				tf.Position = rb.GetPosition();
				tf.Rotation = rb.GetRotation();
				// Push the new world transform back into Local* so future
				// reads of LocalPosition / LocalRotation (inspector, scripts,
				// pre-step compose) see the physics-driven value, not the
				// stale authored one.
				DecomposeBodyOwnedWorldToLocal(registry, ent, tf);
				tf.ClearDirty();
			}

			for (auto [ent, body, tf] : registry.view<FastBody2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (body.m_Body) {
					auto pos = body.m_Body->GetPosition();
					tf.Position = { pos.x, pos.y };
					// Mirror the Rigidbody2D path: keep LocalPosition in sync
					// with the physics-driven world position.
					DecomposeBodyOwnedWorldToLocal(registry, ent, tf);
					tf.ClearDirty();
				}
			}
		});

		auto dispatchToCollisionScenes = [](const Collision2D& collision, const auto& dispatch) {
			if (collision.sceneA && collision.sceneA->IsValid(collision.entityA)) {
				dispatch(*collision.sceneA, collision);
			}
			if (collision.sceneB && collision.sceneB != collision.sceneA && collision.sceneB->IsValid(collision.entityB)) {
				dispatch(*collision.sceneB, collision);
			}
		};

		s_MainWorld->GetDispatcher().Process(
			s_MainWorld->GetWorldID(),
			[world = &*s_MainWorld](b2ShapeId shapeId) { return world->ResolveShape(shapeId); },
			[&dispatchToCollisionScenes](const Collision2D& collision) {
				dispatchToCollisionScenes(collision, [](Scene& scene, const Collision2D& c) {
					ScriptSystem::DispatchCollisionEnter2D(scene, c);
				});
			},
			[&dispatchToCollisionScenes](const Collision2D& collision) {
				dispatchToCollisionScenes(collision, [](Scene& scene, const Collision2D& c) {
					ScriptSystem::DispatchCollisionExit2D(scene, c);
				});
			},
			[&dispatchToCollisionScenes](const Collision2D& collision) {
				dispatchToCollisionScenes(collision, [](Scene& scene, const Collision2D& c) {
					ScriptSystem::DispatchCollisionStay2D(scene, c);
				});
			});

		// Fast* colliders / FastBody2D live in the AxiomPhys world. Without
		// this dispatch the OnCollisionEnter2D / OnCollisionStay2D /
		// OnCollisionExit2D script events never fire for them — the per-entity
		// IndexContactCallback path that IndexPhysicsWorld2D::Step uses
		// isn't wired up to ScriptSystem.
		s_IndexWorld->DispatchScriptContacts(
			[&dispatchToCollisionScenes](const Collision2D& collision) {
				dispatchToCollisionScenes(collision, [](Scene& scene, const Collision2D& c) {
					ScriptSystem::DispatchCollisionEnter2D(scene, c);
				});
			},
			[&dispatchToCollisionScenes](const Collision2D& collision) {
				dispatchToCollisionScenes(collision, [](Scene& scene, const Collision2D& c) {
					ScriptSystem::DispatchCollisionStay2D(scene, c);
				});
			},
			[&dispatchToCollisionScenes](const Collision2D& collision) {
				dispatchToCollisionScenes(collision, [](Scene& scene, const Collision2D& c) {
					ScriptSystem::DispatchCollisionExit2D(scene, c);
				});
			});
	}

	void PhysicsSystem2D::Shutdown() {
		if (s_IndexWorld) {
			s_IndexWorld->Destroy();
			s_IndexWorld.reset();
		}
		s_MainWorld.reset();
	}
}
