#include <doctest/doctest.h>

#include "Collections/Color.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Core/UUID.hpp"
#include "Graphics/Filter.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializer.hpp"

#include <filesystem>
#include <system_error>

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

// Bursts live in ParticleSystem2DComponent's m_Bursts vector and were the regression that prompted
// this canary: SerializeEntity wrote a "bursts" array but DeserializeFullEntity (the scene-load path)
// never read it back — only DeserializeComponent did — so every save/load dropped all bursts. This
// drives the same full-entity ladder used by scene load, binary load and prefab instantiation.
TEST_CASE("Full-entity serialize round-trip preserves ParticleSystem2D bursts")
{
	auto scene = Scene::CreateDetachedScene("RoundTrip Bursts");

	Entity e = scene->CreateEntity("Emitter");

	auto& ps = e.AddComponent<ParticleSystem2DComponent>();
	ps.AddBurst(ParticleSystem2DComponent::Burst{ 25, 0.5f, 0.0f });
	ps.AddBurst(ParticleSystem2DComponent::Burst{ 100, 2.25f, 0.0f });

	const Json::Value value = SceneSerializer::SerializeEntityFull(*scene, e.GetHandle());
	REQUIRE(value.IsObject());

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, value);
	REQUIRE(cloneHandle != entt::null);
	REQUIRE(scene->HasComponent<ParticleSystem2DComponent>(cloneHandle));

	const auto bursts = scene->GetComponent<ParticleSystem2DComponent>(cloneHandle).GetBursts();
	REQUIRE(bursts.size() == 2);
	CHECK(bursts[0].Count == 25);
	CHECK(bursts[0].Interval == doctest::Approx(0.5f));
	CHECK(bursts[1].Count == 100);
	CHECK(bursts[1].Interval == doctest::Approx(2.25f));
}

// Rigidbody2D mass has two distinct states that MUST round-trip independently: an explicit
// user-pinned mass (m_MassOverridden=true) and the default density-based auto mass
// (m_MassOverridden=false). The bug these guard: a prefab/scene saved an explicit mass=100 but on
// reload the collider's shape attach recomputed mass from density*area and discarded it. The fix
// re-asserts a pinned mass after shapes attach, gated by an explicit "massOverridden" flag so
// legacy/auto bodies are NOT forced to a frozen value. Detached scenes create no live b2 body, so
// these exercise the serialize ladder + the override flag, not the live-shape recompute.
TEST_CASE("Full-entity serialize round-trip preserves an explicit Rigidbody2D mass")
{
	auto scene = Scene::CreateDetachedScene("RoundTrip Rigidbody Mass");

	Entity e = scene->CreateEntity("Body");
	auto& rb = e.AddComponent<Rigidbody2DComponent>();
	rb.SetMass(100.0f);
	REQUIRE(rb.IsMassOverridden());

	const Json::Value value = SceneSerializer::SerializeEntityFull(*scene, e.GetHandle());
	REQUIRE(value.IsObject());

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, value);
	REQUIRE(cloneHandle != entt::null);
	REQUIRE(scene->HasComponent<Rigidbody2DComponent>(cloneHandle));

	const auto& restored = scene->GetComponent<Rigidbody2DComponent>(cloneHandle);
	CHECK(restored.IsMassOverridden());
	CHECK(restored.GetMass() == doctest::Approx(100.0f));
}

// Guards the regression flagged during review: marking EVERY deserialized body as overridden would
// freeze auto-mass bodies at a stored value. A rigidbody whose mass was never set must come back
// NOT overridden so it keeps density-based auto mass.
TEST_CASE("Full-entity serialize round-trip keeps a default Rigidbody2D on auto mass")
{
	auto scene = Scene::CreateDetachedScene("RoundTrip Rigidbody AutoMass");

	Entity e = scene->CreateEntity("Body");
	e.AddComponent<Rigidbody2DComponent>(); // mass never set -> auto

	const Json::Value value = SceneSerializer::SerializeEntityFull(*scene, e.GetHandle());
	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, value);
	REQUIRE(cloneHandle != entt::null);
	REQUIRE(scene->HasComponent<Rigidbody2DComponent>(cloneHandle));

	CHECK_FALSE(scene->GetComponent<Rigidbody2DComponent>(cloneHandle).IsMassOverridden());
}

// The inspector reset/paste ladder (DeserializeComponent) must carry the override flag too.
TEST_CASE("Component-level serialize round-trip preserves Rigidbody2D mass override")
{
	auto scene = Scene::CreateDetachedScene("RoundTrip Rigidbody Mass Component");

	Entity src = scene->CreateEntity("Src");
	src.AddComponent<Rigidbody2DComponent>().SetMass(57.5f);

	const Json::Value comp = SceneSerializer::SerializeComponent(*scene, src.GetHandle(), "Rigidbody2D");
	REQUIRE(comp.IsObject());

	Entity dst = scene->CreateEntity("Dst");
	dst.AddComponent<Rigidbody2DComponent>(); // starts at default auto mass
	REQUIRE(SceneSerializer::DeserializeComponent(*scene, dst.GetHandle(), "Rigidbody2D", comp));

	const auto& r = dst.GetComponent<Rigidbody2DComponent>();
	CHECK(r.IsMassOverridden());
	CHECK(r.GetMass() == doctest::Approx(57.5f));
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

// Full-file save/load round-trips. Nothing else in the suite exercises SaveToFile / LoadFromFile,
// and LoadFromFile of a binary scene now takes the streaming decode path (BeginBinaryScene +
// ReadNextBinaryEntity, one entity materialized at a time) — these guard that the streaming path
// and the JSON whole-DOM path both reconstruct entities, hierarchy, child order and components.

namespace {

	EntityHandle FindEntityByName(Scene& scene, const std::string& name) {
		for (auto e : scene.GetRegistry().view<NameComponent>()) {
			if (scene.GetRegistry().get<NameComponent>(e).Name == name) {
				return e;
			}
		}
		return entt::null;
	}

	// Root with two children (ChildA created first → child index 0); ChildA carries a SpriteRenderer.
	void BuildHierarchyScene(Scene& scene) {
		Entity root = scene.CreateEntity("Root");
		root.GetComponent<Transform2DComponent>().LocalPosition = { 1.0f, 2.0f };

		Entity childA = scene.CreateEntity("ChildA");
		childA.GetComponent<Transform2DComponent>().LocalPosition = { 3.0f, 4.0f };
		auto& sprite = childA.AddComponent<SpriteRendererComponent>();
		sprite.Color = Color{ 0.2f, 0.4f, 0.6f, 0.8f };
		sprite.SortingOrder = 11;
		sprite.SpriteName = "child_a_sprite";
		childA.SetParent(root);

		Entity childB = scene.CreateEntity("ChildB");
		childB.GetComponent<Transform2DComponent>().LocalPosition = { 5.0f, 6.0f };
		childB.SetParent(root);
	}

	void VerifyHierarchyScene(Scene& scene) {
		CHECK(scene.GetEntityCount() == 3);

		const EntityHandle rootH = FindEntityByName(scene, "Root");
		const EntityHandle childAH = FindEntityByName(scene, "ChildA");
		const EntityHandle childBH = FindEntityByName(scene, "ChildB");
		REQUIRE(rootH != entt::null);
		REQUIRE(childAH != entt::null);
		REQUIRE(childBH != entt::null);

		Entity root = scene.GetEntity(rootH);
		Entity childA = scene.GetEntity(childAH);
		Entity childB = scene.GetEntity(childBH);

		// Parent links survived.
		CHECK(childA.GetParent().GetHandle() == rootH);
		CHECK(childB.GetParent().GetHandle() == rootH);

		// Authored child order survived (ChildA at index 0, ChildB at 1).
		const std::span<const EntityHandle> children = root.GetChildren();
		REQUIRE(children.size() == 2);
		CHECK(scene.GetEntity(children[0]).GetName() == "ChildA");
		CHECK(scene.GetEntity(children[1]).GetName() == "ChildB");

		// Transforms survived.
		CHECK(root.GetComponent<Transform2DComponent>().LocalPosition.x == doctest::Approx(1.0f));
		CHECK(childB.GetComponent<Transform2DComponent>().LocalPosition.y == doctest::Approx(6.0f));

		// A component on a child survived.
		REQUIRE(scene.HasComponent<SpriteRendererComponent>(childAH));
		const auto& sprite = scene.GetComponent<SpriteRendererComponent>(childAH);
		CHECK(sprite.Color.r == doctest::Approx(0.2f));
		CHECK(sprite.SortingOrder == 11);
		CHECK(sprite.SpriteName == "child_a_sprite");
	}

} // namespace

TEST_CASE("Scene file round-trip (binary streaming) preserves entities, hierarchy, components")
{
	const std::filesystem::path path =
		std::filesystem::temp_directory_path() / "index_test_scene_binary.scene";

	{
		auto saveScene = Scene::CreateDetachedScene("StreamSaveBinary");
		BuildHierarchyScene(*saveScene);
		REQUIRE(SceneSerializer::SaveToFile(*saveScene, path.string(), SceneSerializationFormat::Binary));
	}

	// Confirms the file really is binary, so LoadFromFile takes the streaming decode path.
	CHECK(SceneSerializer::IsBinarySerializedFile(path.string()));

	auto loadScene = Scene::CreateDetachedScene("StreamLoadBinary");
	REQUIRE(SceneSerializer::LoadFromFile(*loadScene, path.string()));
	VerifyHierarchyScene(*loadScene);

	std::error_code ec;
	std::filesystem::remove(path, ec);
}

TEST_CASE("Scene file round-trip (JSON) preserves entities, hierarchy, components")
{
	const std::filesystem::path path =
		std::filesystem::temp_directory_path() / "index_test_scene_json.scene";

	{
		auto saveScene = Scene::CreateDetachedScene("StreamSaveJson");
		BuildHierarchyScene(*saveScene);
		REQUIRE(SceneSerializer::SaveToFile(*saveScene, path.string(), SceneSerializationFormat::Json));
	}

	CHECK_FALSE(SceneSerializer::IsBinarySerializedFile(path.string()));

	auto loadScene = Scene::CreateDetachedScene("StreamLoadJson");
	REQUIRE(SceneSerializer::LoadFromFile(*loadScene, path.string()));
	VerifyHierarchyScene(*loadScene);

	std::error_code ec;
	std::filesystem::remove(path, ec);
}
