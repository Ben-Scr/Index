#include <doctest/doctest.h>

#include "Collections/Vec2.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Math/Trigonometry.hpp"
#include "Scene/Entity.hpp"
#include "Scene/EntityPicker.hpp"
#include "Scene/Scene.hpp"

#include <vector>

using namespace Index;

// Texture-alpha precision needs a decoded texture on disk and isn't exercisable
// headlessly, so these cover the oriented-quad (rotation-aware) path and the
// topmost-first ordering that drives the editor's click-to-cycle behaviour.

TEST_CASE("Picking a rotated entity respects its oriented quad, not its AABB")
{
	auto scene = Scene::CreateDetachedScene("Picker Rotated");

	Entity e = scene->CreateEntity("Rotated");
	auto& tx = e.GetComponent<Transform2DComponent>();
	tx.Position = Vec2{ 0.0f, 0.0f };
	tx.Scale = Vec2{ 1.0f, 1.0f };
	tx.Rotation = Radians<float>(45.0f);

	EntityHandle picked = entt::null;

	// Center of the quad is always inside.
	REQUIRE(EntityPicker::TryPickEntity(*scene, Vec2{ 0.0f, 0.0f }, picked));
	CHECK(picked == e.GetHandle());

	// (0.65, 0.65) lies inside the rotated unit square's AABB (half-extent ~0.707)
	// but outside the rotated quad itself — the old AABB picker would have hit it.
	CHECK_FALSE(EntityPicker::TryPickEntity(*scene, Vec2{ 0.65f, 0.65f }, picked));

	// A point along the quad's local +X axis (which the rotation carries to a
	// diagonal) stays inside.
	CHECK(EntityPicker::TryPickEntity(*scene, Vec2{ 0.35f, 0.35f }, picked));
}

TEST_CASE("PickEntitiesAtPoint returns overlapping entities sorted topmost-first")
{
	auto scene = Scene::CreateDetachedScene("Picker Stack");

	auto make = [&](const char* name, uint8_t layer, short order) {
		Entity e = scene->CreateEntity(name);
		auto& tx = e.GetComponent<Transform2DComponent>();
		tx.Position = Vec2{ 0.0f, 0.0f };
		tx.Scale = Vec2{ 2.0f, 2.0f };
		tx.Rotation = 0.0f;
		auto& sprite = e.AddComponent<SpriteRendererComponent>();
		sprite.SortingLayer = layer;
		sprite.SortingOrder = order;
		return e.GetHandle();
	};

	// All three overlap the origin; topmost = highest layer, then highest order.
	const EntityHandle bottom = make("Bottom", 0, 0);
	const EntityHandle mid = make("Mid", 0, 5);
	const EntityHandle top = make("Top", 1, 0);

	std::vector<EntityHandle> stack;
	EntityPicker::PickEntitiesAtPoint(*scene, Vec2{ 0.0f, 0.0f }, stack, /*pixelPrecise*/ false);

	REQUIRE(stack.size() == 3);
	CHECK(stack[0] == top);
	CHECK(stack[1] == mid);
	CHECK(stack[2] == bottom);

	// TryPickEntity returns that same topmost entity.
	EntityHandle picked = entt::null;
	REQUIRE(EntityPicker::TryPickEntity(*scene, Vec2{ 0.0f, 0.0f }, picked));
	CHECK(picked == top);

	// A point outside every quad (quads span [-1, 1]) hits nothing.
	std::vector<EntityHandle> empty;
	EntityPicker::PickEntitiesAtPoint(*scene, Vec2{ 5.0f, 5.0f }, empty, /*pixelPrecise*/ false);
	CHECK(empty.empty());
}
