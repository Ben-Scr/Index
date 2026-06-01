#include "pch.hpp"
#include "Systems/TransformHierarchySystem.hpp"
#include "Scene/Scene.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/Physics/BoxCollider2DComponent.hpp"
#include "Components/Physics/CircleCollider2DComponent.hpp"
#include "Components/Physics/PolygonCollider2DComponent.hpp"
#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Components/Physics/FastBody2DComponent.hpp"
#include "Components/Tags.hpp"
#include "Profiling/Profiler.hpp"

namespace Index {
	namespace {
		// Hard cap recursion depth so a corrupted hierarchy can't infinite-loop.
		constexpr int kMaxHierarchyDepth = 1024;

		void ProcessSubtree(entt::registry& registry, EntityHandle entity,
			const Transform2DComponent* parentWorld, int depth)
		{
			if (depth > kMaxHierarchyDepth) return;
			if (!registry.valid(entity)) return;

			Transform2DComponent* tr = registry.try_get<Transform2DComponent>(entity);
			if (!tr) return;

			const bool ownedByPhysics = registry.any_of<Rigidbody2DComponent, FastBody2DComponent>(entity);
			if (!ownedByPhysics) {
				if (parentWorld) {
					tr->Position = parentWorld->TransformPoint(tr->LocalPosition);
					tr->Rotation = parentWorld->Rotation + tr->LocalRotation;
					tr->Scale = { parentWorld->Scale.x * tr->LocalScale.x,
								  parentWorld->Scale.y * tr->LocalScale.y };
				}
				else {
					tr->Position = tr->LocalPosition;
					tr->Rotation = tr->LocalRotation;
					tr->Scale = tr->LocalScale;
				}
			}

			if (registry.all_of<HierarchyComponent>(entity)) {
				// Snapshot children so a script mutating the hierarchy during the pass doesn't invalidate the iterator.
				const auto childrenSnapshot = registry.get<HierarchyComponent>(entity).Children;
				for (EntityHandle child : childrenSnapshot) {
					ProcessSubtree(registry, child, tr, depth + 1);
				}
			}

			if (!ownedByPhysics
				&& !registry.any_of<BoxCollider2DComponent, CircleCollider2DComponent, PolygonCollider2DComponent>(entity)) {
				tr->ClearDirty();
			}
		}

		void ProcessRootTransform(entt::registry& registry, EntityHandle entity) {
			if (!registry.valid(entity)) return;
			auto* tr = registry.try_get<Transform2DComponent>(entity);
			if (!tr) return;

			const bool ownedByPhysics = registry.any_of<Rigidbody2DComponent, FastBody2DComponent>(entity);
			if (!ownedByPhysics) {
				tr->Position = tr->LocalPosition;
				tr->Rotation = tr->LocalRotation;
				tr->Scale = tr->LocalScale;
			}

			if (!ownedByPhysics
				&& !registry.any_of<BoxCollider2DComponent, CircleCollider2DComponent, PolygonCollider2DComponent>(entity)) {
				tr->ClearDirty();
			}
		}

		bool HasBodyDrivenHierarchy(entt::registry& registry) {
			auto rigidbodyView = registry.view<Rigidbody2DComponent, HierarchyComponent>(entt::exclude<DisabledTag>);
			for (auto entity : rigidbodyView) {
				if (!registry.get<HierarchyComponent>(entity).Children.empty()) return true;
			}

			auto fastBodyView = registry.view<FastBody2DComponent, HierarchyComponent>(entt::exclude<DisabledTag>);
			for (auto entity : fastBodyView) {
				if (!registry.get<HierarchyComponent>(entity).Children.empty()) return true;
			}

			return false;
		}

		void RunPropagation(Scene& scene) {
			auto& registry = scene.GetRegistry();

			const bool hasHierarchy = registry.view<HierarchyComponent>().size() > 0;
			const bool forceBodyHierarchy = hasHierarchy && HasBodyDrivenHierarchy(registry);
			if (!scene.HasDirtyTransforms() && !forceBodyHierarchy) {
				return;
			}

			// Non-zero profiler reading here means something marks transforms dirty every frame.
			INDEX_PROFILE_SCOPE("TransformHierarchy");

			std::vector<EntityHandle> dirtyTransforms = scene.ConsumeDirtyTransformEntities();

			if (!hasHierarchy) {
				for (EntityHandle entity : dirtyTransforms) {
					ProcessRootTransform(registry, entity);
				}
				return;
			}

			auto orphanRoots = registry.view<Transform2DComponent>(entt::exclude<HierarchyComponent>);
			for (auto entity : orphanRoots) {
				ProcessSubtree(registry, entity, nullptr, 0);
			}

			auto parented = registry.view<Transform2DComponent, HierarchyComponent>();
			for (auto entity : parented) {
				if (parented.get<HierarchyComponent>(entity).Parent != entt::null) {
					continue;
				}
				ProcessSubtree(registry, entity, nullptr, 0);
			}
		}
	}

	void TransformHierarchySystem::Awake(Scene& scene) {
		// One-shot at scene awake so post-load world values are valid for
		// scripts' Awake/Start hooks.
		RunPropagation(scene);
	}

	void TransformHierarchySystem::Update(Scene& scene) {
		RunPropagation(scene);
	}

	void TransformHierarchySystem::OnPreRender(Scene& scene) {
		// Pre-render runs in both edit and play modes (Update is gated by play
		// mode), so this is the hook that keeps children visually following a
		// parent dragged in the inspector while the scene isn't ticking.
		RunPropagation(scene);
	}

	void TransformHierarchySystem::Propagate(Scene& scene) {
		RunPropagation(scene);
	}
}
