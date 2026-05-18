#pragma once
#include <bit>
#include <entt/entt.hpp>

namespace Index {
	using EntityHandle = entt::entity;

	// Sentinel for "no entity". Component headers that need to default-
	// initialise an EntityHandle field can reach this without pulling
	// <entt/entt.hpp> into their own include graph (this header already
	// has it). Editing a UI component header used to cascade an EnTT
	// reparse through every consumer; now it doesn't.
	inline constexpr EntityHandle kNullEntity = entt::null;

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