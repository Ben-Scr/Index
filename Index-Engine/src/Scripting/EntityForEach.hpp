#pragma once

#include "Core/Export.hpp"
#include "Jobs/ParallelFor.hpp"
#include "Scene/EntityHandle.hpp"
#include "Scene/Scene.hpp"

#include <type_traits>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

namespace Index {

	namespace Detail {

		template <typename Body, typename View, typename... Components>
		inline void InvokeForEachBody(Body& body, View& view, EntityHandle entity) {
			if constexpr (std::is_invocable_v<Body&, EntityHandle, Components&...>) {
				body(entity, view.template get<Components>(entity)...);
			}
			else if constexpr (std::is_invocable_v<Body&, Components&...>) {
				body(view.template get<Components>(entity)...);
			}
			else {
				static_assert(std::is_invocable_v<Body&, Components&...>,
					"ForEach body must be invocable with (Components&...) or "
					"(EntityHandle, Components&...).");
			}
		}

	} // namespace Detail

	template <typename... Components, typename Body>
	void ForEach(Scene& scene, Body&& body) {
		auto view = scene.GetRegistry().view<Components...>();
		for (auto entity : view) {
			Detail::InvokeForEachBody<Body, decltype(view), Components...>(
				body, view, entity);
		}
	}

	template <typename... Components, typename Body>
	void ParallelForEach(Scene& scene, Body&& body, std::size_t grainSize = 0) {
		auto view = scene.GetRegistry().view<Components...>();

		std::vector<EntityHandle> entities;
		entities.reserve(static_cast<std::size_t>(view.size_hint()));
		for (auto entity : view) {
			entities.push_back(entity);
		}

		if (entities.empty()) {
			return;
		}

		ParallelFor(0, entities.size(),
			[&](std::size_t lo, std::size_t hi) {
				for (std::size_t i = lo; i < hi; ++i) {
					Detail::InvokeForEachBody<Body, decltype(view), Components...>(
						body, view, entities[i]);
				}
			},
			grainSize);
	}

} // namespace Index
