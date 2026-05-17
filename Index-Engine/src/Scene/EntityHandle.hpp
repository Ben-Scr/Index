#pragma once
#include <bit>
#include <entt/entt.hpp>

namespace Index {
	using EntityHandle = entt::entity;

	// Bit width of the entity-index portion of EntityHandle baked into the
	// engine at compile time. Equal to the project's `entityBits` setting at
	// the time the engine was last built. The editor compares this against
	// the in-memory IndexProject::EntityBits to detect drift after the user
	// changes the dropdown without rebuilding, and surfaces a "Rebuild
	// Engine" button when they disagree.
	constexpr int GetCompiledEntityBits() noexcept {
		return std::popcount(entt::entt_traits<EntityHandle>::entity_mask);
	}
}