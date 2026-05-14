#include <doctest/doctest.h>

#include "Assets/AssetRegistry.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/PrefabInstanceComponent.hpp"
#include "Components/Tags.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Serialization/SceneSerializer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace Index;

TEST_CASE("Entity null component access is guarded")
{
	Entity entity = Entity::Null;
	CHECK_FALSE(entity.IsValid());
	CHECK_FALSE(entity.HasComponent<NameComponent>());
	CHECK_FALSE(entity.HasAnyComponent<NameComponent>());

	NameComponent* name = nullptr;
	CHECK_FALSE(entity.TryGetComponent(name));
	CHECK(name == nullptr);
	CHECK_NOTHROW(entity.RemoveComponent<NameComponent>());

	CHECK_THROWS(entity.GetComponent<NameComponent>());
	CHECK_THROWS(entity.AddComponent<NameComponent>(std::string("Name")));

	const Entity constEntity = Entity::Null;
	CHECK_THROWS(constEntity.GetComponent<NameComponent>());
	CHECK(constEntity.GetName().find("Unnamed Entity") == 0);
}

TEST_CASE("Empty entity names do not create NameComponent")
{
	auto scene = Scene::CreateDetachedScene("Empty Name Test");

	Entity editorEntity = scene->CreateEntity("");
	CHECK(editorEntity.IsValid());
	CHECK_FALSE(editorEntity.HasComponent<NameComponent>());

	Entity runtimeEntity = scene->CreateRuntimeEntity("");
	CHECK(runtimeEntity.IsValid());
	CHECK_FALSE(runtimeEntity.HasComponent<NameComponent>());

	Entity namedEntity = scene->CreateEntity("Named");
	REQUIRE(namedEntity.HasComponent<NameComponent>());
	CHECK(namedEntity.GetComponent<NameComponent>().Name == "Named");
}

TEST_CASE("Parent enable preserves child authored disabled state")
{
	auto scene = Scene::CreateDetachedScene("Disabled Hierarchy Test");

	Entity parent = scene->CreateEntity("Parent");
	Entity child = scene->CreateEntity("Child");
	child.SetParent(parent);
	child.SetEnabled(false);

	parent.SetEnabled(false);
	parent.SetEnabled(true);

	CHECK_FALSE(parent.HasComponent<DisabledTag>());
	CHECK(child.HasComponent<DisabledTag>());
	CHECK_FALSE(child.HasComponent<InheritedDisabledTag>());
}

TEST_CASE("Parent enable clears inherited disabled state")
{
	auto scene = Scene::CreateDetachedScene("Inherited Disabled Test");

	Entity parent = scene->CreateEntity("Parent");
	Entity child = scene->CreateEntity("Child");
	child.SetParent(parent);

	parent.SetEnabled(false);
	CHECK(child.HasComponent<DisabledTag>());
	CHECK(child.HasComponent<InheritedDisabledTag>());

	parent.SetEnabled(true);
	CHECK_FALSE(parent.HasComponent<DisabledTag>());
	CHECK_FALSE(child.HasComponent<DisabledTag>());
	CHECK_FALSE(child.HasComponent<InheritedDisabledTag>());
}

TEST_CASE("Prefab instantiation rescans registry for late-created prefab assets")
{
	namespace fs = std::filesystem;

	const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("IndexPrefabInstantiateTest_" + std::to_string(uniqueSuffix));
	const fs::path assets = root / "Assets";
	const fs::path prefabPath = assets / "LatePrefab.prefab";
	const uint64_t prefabGuid = 1204567890123456789ull;

	struct Cleanup {
		fs::path Root;
		~Cleanup()
		{
			ProjectManager::SetCurrentProject(std::unique_ptr<IndexProject>());
			std::error_code ec;
			fs::remove_all(Root, ec);
		}
	} cleanup{ root };

	fs::create_directories(assets);

	auto project = std::make_unique<IndexProject>();
	project->Name = "Prefab Instantiate Test";
	project->RootDirectory = root.string();
	project->AssetsDirectory = assets.string();
	ProjectManager::SetCurrentProject(std::move(project));

	// Create the prefab after ProjectManager::SetCurrentProject has already
	// synced the empty Assets directory. This mirrors editor paths that mint
	// prefab assets after one AssetRegistry copy has gone clean.
	{
		std::ofstream prefab(prefabPath);
		REQUIRE(prefab.is_open());
		prefab
			<< "{\n"
			<< "  \"version\": 1,\n"
			<< "  \"type\": \"Prefab\",\n"
			<< "  \"Entity\": {\n"
			<< "    \"name\": \"Late Prefab\",\n"
			<< "    \"uuid\": \"987654321\",\n"
			<< "    \"Transform2D\": { \"posX\": 4, \"posY\": 5, \"rotation\": 0, \"scaleX\": 1, \"scaleY\": 1 }\n"
			<< "  }\n"
			<< "}\n";
	}
	{
		std::ofstream meta(prefabPath.string() + std::string(AssetRegistry::MetaExtension));
		REQUIRE(meta.is_open());
		meta
			<< "{\n"
			<< "  \"AssetGUID\": \"" << prefabGuid << "\",\n"
			<< "  \"kind\": \"Prefab\"\n"
			<< "}\n";
	}

	auto scene = Scene::CreateDetachedScene("Prefab Runtime Test");
	const EntityHandle instance = SceneSerializer::InstantiatePrefab(*scene, prefabGuid);
	REQUIRE(instance != entt::null);
	REQUIRE(scene->IsValid(instance));

	CHECK(scene->GetEntityOrigin(instance) == EntityOrigin::Prefab);
	CHECK(static_cast<uint64_t>(scene->GetPrefabGUID(instance)) == prefabGuid);
	CHECK(scene->HasComponent<PrefabInstanceComponent>(instance));
	CHECK(scene->GetComponent<NameComponent>(instance).Name == "Late Prefab");
}
