#pragma once
#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Tags.hpp"
#include "Scene/EntityHandle.hpp"

#include <entt/entt.hpp>
#include <utility>
#include <vector>

namespace Index::UIDrawOrder {

	// DrawIndex stride per RectTransform2D. GuiRenderer slots per-widget
	// overlays (input field selection highlight, caret) between an entity
	// and its first child without a separate sort key, so the walk steps
	// by 4 instead of 1 to keep the index space free for those overlays.
	inline constexpr int k_HierarchyStep = 4;

	// Recursive hierarchy walk: every RectTransform2D entity gets a
	// monotonically increasing DrawIndex. Earlier siblings get lower
	// indices, deeper descendants follow their parent. Disabled subtrees
	// are skipped entirely so they neither render nor hit-test.
	inline void Collect(entt::registry& registry, EntityHandle entity,
		std::vector<std::pair<EntityHandle, int>>& outOrder, int& counter)
	{
		if (registry.all_of<DisabledTag>(entity)) return;
		if (registry.all_of<RectTransform2DComponent>(entity)) {
			outOrder.emplace_back(entity, counter);
			counter += k_HierarchyStep;
		}
		if (auto* hierarchy = registry.try_get<HierarchyComponent>(entity)) {
			for (EntityHandle child : hierarchy->Children) {
				if (registry.valid(child)) {
					Collect(registry, child, outOrder, counter);
				}
			}
		}
	}

	// Build the full UI draw order for a registry: roots-in-creation-order,
	// then a `Collect` walk under each. Output is sorted by paint order:
	// earlier entries render behind, later entries render on top.
	//
	// GuiRenderer feeds this list into its image / text scratch buffers and
	// sorts those by (SortingLayer, SortingOrder, DrawIndex). UIEventSystem
	// uses the same list to pick the front-most Interactable rect under the
	// cursor — the renderer's z-stack and the hit-test stack must agree, so
	// both go through this single helper.
	inline void Build(entt::registry& registry,
		std::vector<std::pair<EntityHandle, int>>& outOrder)
	{
		std::vector<EntityHandle> roots;
		roots.reserve(32);
		auto allView = registry.view<entt::entity>(entt::exclude<DisabledTag>);
		for (auto entity : allView) {
			const HierarchyComponent* hc = registry.try_get<HierarchyComponent>(entity);
			const bool isRoot = !hc || hc->Parent == entt::null;
			if (!isRoot) continue;
			roots.push_back(entity);
		}
		// EnTT iterates dense storage newest-first. Reverse so the oldest
		// root walks first and gets the lowest DrawIndex — matches the
		// "newer-on-top" mental model the rest of the engine uses.
		std::reverse(roots.begin(), roots.end());

		int counter = 0;
		for (EntityHandle entity : roots) {
			Collect(registry, entity, outOrder, counter);
		}
	}

}
