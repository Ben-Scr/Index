#pragma once
#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Tags.hpp"
#include "Scene/EntityHandle.hpp"

#include <entt/entt.hpp>
#include <algorithm>
#include <unordered_set>
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
	//
	// Root discovery walks UP from each RectTransform2D entity rather than
	// iterating the whole registry. Cost is O(uiCount × averageDepth) —
	// independent of non-UI entity count, so a scene with 100k gameplay
	// entities + a small HUD pays only for the HUD entities.
	inline void Build(entt::registry& registry,
		std::vector<std::pair<EntityHandle, int>>& outOrder)
	{
		// Fast path: no UI at all means no draw order. view<T>::size() on
		// a single-component view is O(1).
		auto uiView = registry.view<RectTransform2DComponent>();
		if (uiView.size() == 0) return;

		// Walk up from each UI entity to its hierarchy root, deduplicating
		// via an unordered_set so multiple UI children of the same root
		// don't enqueue it twice. Skips entities whose ancestor chain
		// contains a DisabledTag — Collect would prune them anyway, so the
		// roots derived from them produce no output and discarding them
		// here avoids walking the dead subtree.
		std::unordered_set<EntityHandle> rootSet;
		rootSet.reserve(32);

		for (auto entity : uiView) {
			if (registry.all_of<DisabledTag>(entity)) continue;

			EntityHandle cur = entity;
			bool ancestorDisabled = false;
			// Depth guard against pathological / cyclic parent chains.
			// Real hierarchies are nowhere near this; the bound just
			// stops a corrupt scene file from hanging the render thread.
			for (int hop = 0; hop < 4096; ++hop) {
				const HierarchyComponent* hc = registry.try_get<HierarchyComponent>(cur);
				if (!hc || hc->Parent == entt::null) break;
				if (!registry.valid(hc->Parent)) break;
				if (registry.all_of<DisabledTag>(hc->Parent)) {
					ancestorDisabled = true;
					break;
				}
				cur = hc->Parent;
			}
			if (!ancestorDisabled) rootSet.insert(cur);
		}

		if (rootSet.empty()) return;

		// Sort roots by EnTT dense-storage index to preserve the
		// pre-refactor z-order convention: older entities paint behind
		// newer ones. sparse_set::index is O(1). The previous code
		// achieved the same order via reverse-iterate-then-reverse on
		// the entt::entity view; sorting by ascending dense index here
		// is equivalent and skips the all-entity walk.
		const auto& entityStorage = registry.storage<entt::entity>();
		std::vector<EntityHandle> roots(rootSet.begin(), rootSet.end());
		std::sort(roots.begin(), roots.end(),
			[&](EntityHandle a, EntityHandle b) {
				return entityStorage.index(a) < entityStorage.index(b);
			});

		int counter = 0;
		for (EntityHandle entity : roots) {
			Collect(registry, entity, outOrder, counter);
		}
	}

}
