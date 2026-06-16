#include <doctest/doctest.h>

#include "Collections/Color.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Core/UUID.hpp"
#include "Graphics/Filter.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializer.hpp"

using namespace Index;

// These guard the audit's #1 risk: per-component serialize logic is hand-duplicated across THREE
// ladders that must stay in lock-step — SerializeEntity (SceneSerializer.cpp), DeserializeFullEntity
// (the scene-load path) and DeserializeComponent (the inspector reset/paste path). A field added to
// one ladder but not the others is silently dropped on save/load, and nothing else in the suite
// exercises a full round-trip. SpriteRenderer is the densest hand-serialized data component, so it is
// the best canary. TextureAssetId is left 0 so no real texture/GPU resource is needed (the manager
// lookups are null-safe for invalid handles, keeping these headless).

TEST_CASE("Full-entity serialize round-trip preserves SpriteRenderer + Transform + Name")
{
	auto scene = Scene::CreateDetachedScene("RoundTrip Full Entity");

	Entity e = scene->CreateEntity("Hero");

	auto& transform = e.GetComponent<Transform2DComponent>();
	transform.LocalPosition = { 2.0f, 3.0f };
	transform.Position = transform.LocalPosition;

	auto& sprite = e.AddComponent<SpriteRendererComponent>();
	sprite.Color = Color{ 0.25f, 0.5f, 0.75f, 0.9f };
	sprite.SortingOrder = 42;
	sprite.SortingLayer = 7;
	sprite.FilterMode = Filter::Trilinear;
	sprite.SpriteName = "hero_walk_02";

	const Json::Value value = SceneSerializer::SerializeEntityFull(*scene, e.GetHandle());
	REQUIRE(value.IsObject());

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, value);
	REQUIRE(cloneHandle != entt::null);
	REQUIRE(scene->IsValid(cloneHandle));

	Entity clone = scene->GetEntity(cloneHandle);
	CHECK(clone.GetName() == "Hero");

	REQUIRE(scene->HasComponent<SpriteRendererComponent>(cloneHandle));
	const auto& restored = scene->GetComponent<SpriteRendererComponent>(cloneHandle);
	CHECK(restored.Color.r == doctest::Approx(0.25f));
	CHECK(restored.Color.g == doctest::Approx(0.5f));
	CHECK(restored.Color.b == doctest::Approx(0.75f));
	CHECK(restored.Color.a == doctest::Approx(0.9f));
	CHECK(restored.SortingOrder == 42);
	CHECK(static_cast<int>(restored.SortingLayer) == 7);
	CHECK(restored.FilterMode == Filter::Trilinear);
	CHECK(restored.SpriteName == "hero_walk_02");

	const auto& clonedTransform = clone.GetComponent<Transform2DComponent>();
	CHECK(clonedTransform.LocalPosition.x == doctest::Approx(2.0f));
	CHECK(clonedTransform.LocalPosition.y == doctest::Approx(3.0f));
}

TEST_CASE("Component-level serialize round-trip preserves SpriteRenderer fields")
{
	auto scene = Scene::CreateDetachedScene("RoundTrip Component");

	Entity src = scene->CreateEntity("Src");
	auto& s = src.AddComponent<SpriteRendererComponent>();
	s.Color = Color{ 0.1f, 0.2f, 0.3f, 0.4f };
	s.SortingOrder = -13;   // signed: exercises short round-trip, not just unsigned
	s.SortingLayer = 4;
	s.FilterMode = Filter::Anisotropic;
	s.SpriteName = "fx_explosion";

	const Json::Value comp = SceneSerializer::SerializeComponent(*scene, src.GetHandle(), "SpriteRenderer");
	REQUIRE(comp.IsObject());

	Entity dst = scene->CreateEntity("Dst");
	dst.AddComponent<SpriteRendererComponent>(); // starts at defaults
	REQUIRE(SceneSerializer::DeserializeComponent(*scene, dst.GetHandle(), "SpriteRenderer", comp));

	const auto& r = dst.GetComponent<SpriteRendererComponent>();
	CHECK(r.Color.r == doctest::Approx(0.1f));
	CHECK(r.Color.g == doctest::Approx(0.2f));
	CHECK(r.Color.b == doctest::Approx(0.3f));
	CHECK(r.Color.a == doctest::Approx(0.4f));
	CHECK(r.SortingOrder == -13);
	CHECK(static_cast<int>(r.SortingLayer) == 4);
	CHECK(r.FilterMode == Filter::Anisotropic);
	CHECK(r.SpriteName == "fx_explosion");
}
