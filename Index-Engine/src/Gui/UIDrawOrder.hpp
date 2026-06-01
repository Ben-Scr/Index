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

	// Stride 4 (not 1) so GuiRenderer can slot per-widget overlay draws (selection, caret) between a parent and its first child without a separate key.
	inline constexpr int k_HierarchyStep = 4;

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

	// GuiRenderer and UIEventSystem both consume this list — the renderer's z-stack and the hit-test stack MUST agree, so both go through this single helper.
	inline void Build(entt::registry& registry,
		std::vector<std::pair<EntityHandle, int>>& outOrder)
	{
		auto uiView = registry.view<RectTransform2DComponent>();
		if (uiView.size() == 0) return;

		std::unordered_set<EntityHandle> rootSet;
		rootSet.reserve(32);

		for (auto entity : uiView) {
			if (registry.all_of<DisabledTag>(entity)) continue;

			EntityHandle cur = entity;
			bool ancestorDisabled = false;
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
