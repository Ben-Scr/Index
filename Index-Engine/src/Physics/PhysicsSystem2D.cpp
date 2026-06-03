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
		// TransformHierarchySystem skips body-owned entities, so inspector edits (LocalPosition only) must be composed to world before the body sync reads tf.Position.
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

		// After each physics step, write Local* back from the physics-driven world transform so inspector/script reads stay consistent.
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
		IDX_CORE_INFO_TAG("PhysicsSystem", "Box2D + Index-Physics initialized");
	}

	void PhysicsSystem2D::SyncTransformsToPhysics() {
		// Collider shape rebuild FIRST: body loops clear the dirty flag, so shape sync must read it before they do.
		SceneManager::Get().ForeachLoadedScene([](Scene& scene) {
			auto& registry = scene.GetRegistry();

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

			// IsDirty gate dropped here: scene deserialization writes Transform2D
			// fields directly without flipping the dirty flag, and the FastBox /
			// FastCircle OnConstruct hooks may fire before the entity's
			// Transform2D scale is populated. Without an unconditional sync the
			// collider would stay at whatever extents construct snapshotted.
			// SyncWithTransform itself short-circuits when scale equals
			// m_LastAppliedScale, so the per-frame cost is one Vec2 compare.
			for (auto [ent, box, tf] : registry.view<FastBoxCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (box.IsValid()) {
					box.SyncWithTransform(scene);
				}
			}

			for (auto [ent, circle, tf] : registry.view<FastCircleCollider2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (circle.IsValid()) {
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
		// Editor mode needs this; in play mode FixedUpdate already covers the sync so skip to avoid double-walking dirty flags.
		if (Application::GetIsPlaying() && !Application::IsPaused()) {
			return;
		}
		SyncTransformsToPhysics();
	}

	/* static */ void PhysicsSystem2D::WakeAllBodies() {
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

		s_MainWorld->Step(dt);
		s_IndexWorld->Step(dt);

		SceneManager::Get().ForeachLoadedScene([](Scene& scene) {
			auto& registry = scene.GetRegistry();

			for (auto [ent, rb, tf] : registry.view<Rigidbody2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (!rb.IsValid()) continue;
				tf.Position = rb.GetPosition();
				tf.Rotation = rb.GetRotation();
				DecomposeBodyOwnedWorldToLocal(registry, ent, tf);
				tf.ClearDirty();
			}

			for (auto [ent, body, tf] : registry.view<FastBody2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (body.m_Body) {
					auto pos = body.m_Body->GetPosition();
					tf.Position = { pos.x, pos.y };
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
