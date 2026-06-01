#include <doctest/doctest.h>

#include "Assets/AssetRegistry.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/PrefabInstanceComponent.hpp"
#include "Components/Tags.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/SceneSerializerShared.hpp"

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

TEST_CASE("Dragging an entity hierarchy to a prefab marks every entity as a prefab instance")
{
	// Regression: SaveEntityAsPrefabInstance must convert the whole subtree, not just the dragged root.
	namespace fs = std::filesystem;

	const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("IndexPrefabDragTest_" + std::to_string(uniqueSuffix));
	const fs::path assets = root / "Assets";

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
	project->Name = "Prefab Drag Test";
	project->RootDirectory = root.string();
	project->AssetsDirectory = assets.string();
	ProjectManager::SetCurrentProject(std::move(project));

	auto scene = Scene::CreateDetachedScene("Prefab Drag Source");
	Entity parent = scene->CreateEntity("Square");
	Entity child = scene->CreateEntity("Square Child");
	child.SetParent(parent);

	const std::string prefabPath = (assets / "Square.prefab").string();
	const uint64_t prefabGuid =
		SceneSerializer::SaveEntityAsPrefabInstance(*scene, parent.GetHandle(), prefabPath);
	REQUIRE(prefabGuid != 0);

	// Root: converted into an instance of the freshly-written prefab.
	CHECK(scene->GetEntityOrigin(parent.GetHandle()) == EntityOrigin::Prefab);
	CHECK(static_cast<uint64_t>(scene->GetPrefabGUID(parent.GetHandle())) == prefabGuid);
	REQUIRE(scene->HasComponent<PrefabInstanceComponent>(parent.GetHandle()));

	// Child: the reported bug — it must be converted too, not left behind as a
	// plain scene entity, and must point at the same prefab asset.
	CHECK(scene->GetEntityOrigin(child.GetHandle()) == EntityOrigin::Prefab);
	CHECK(static_cast<uint64_t>(scene->GetPrefabGUID(child.GetHandle())) == prefabGuid);
	REQUIRE(scene->HasComponent<PrefabInstanceComponent>(child.GetHandle()));

	// Each entity keeps a distinct, non-zero SourceEntityId so prefab
	// Apply/Revert can pair the instance entities with their template rows.
	const uint64_t rootSourceId =
		scene->GetComponent<PrefabInstanceComponent>(parent.GetHandle()).SourceEntityId;
	const uint64_t childSourceId =
		scene->GetComponent<PrefabInstanceComponent>(child.GetHandle()).SourceEntityId;
	CHECK(rootSourceId != 0);
	CHECK(childSourceId != 0);
	CHECK(rootSourceId != childSourceId);
}

TEST_CASE("Refreshing a prefab child instance does not corrupt the live instance root")
{
	// Regression: RefreshPrefabInstance must be a no-op on non-root entities; handing it a child caused the child's name to overwrite the rebuilt root.
	namespace fs = std::filesystem;

	const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("IndexPrefabChildRefresh_" + std::to_string(uniqueSuffix));
	const fs::path assets = root / "Assets";

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
	project->Name = "Prefab Child Refresh Test";
	project->RootDirectory = root.string();
	project->AssetsDirectory = assets.string();
	ProjectManager::SetCurrentProject(std::move(project));

	// Author a prefab with a root + one named child via the real save path (it
	// writes the ".prefab", which classifies as AssetKind::Prefab).
	auto authorScene = Scene::CreateDetachedScene("Prefab Author");
	Entity authorRoot = authorScene->CreateEntity("RootEntity");
	Entity authorChild = authorScene->CreateEntity("ChildEntity");
	authorChild.SetParent(authorRoot);

	const std::string prefabPath = (assets / "WithChild.prefab").string();
	const uint64_t prefabGuid =
		SceneSerializer::SaveEntityAsPrefabInstance(*authorScene, authorRoot.GetHandle(), prefabPath);
	REQUIRE(prefabGuid != 0);

	// Spawn an independent live instance of the prefab in a separate scene.
	auto gameScene = Scene::CreateDetachedScene("Game Scene");
	const EntityHandle instanceRoot = SceneSerializer::InstantiatePrefab(*gameScene, prefabGuid);
	REQUIRE(instanceRoot != entt::null);
	REQUIRE(gameScene->HasComponent<HierarchyComponent>(instanceRoot));
	REQUIRE(gameScene->GetComponent<HierarchyComponent>(instanceRoot).Children.size() == 1);
	const EntityHandle instanceChild =
		gameScene->GetComponent<HierarchyComponent>(instanceRoot).Children.front();
	REQUIRE(instanceChild != entt::null);

	const uint64_t rootSourceId =
		gameScene->GetComponent<PrefabInstanceComponent>(instanceRoot).SourceEntityId;
	const uint64_t childSourceId =
		gameScene->GetComponent<PrefabInstanceComponent>(instanceChild).SourceEntityId;
	REQUIRE(rootSourceId != 0);
	REQUIRE(childSourceId != 0);
	REQUIRE(rootSourceId != childSourceId);

	Json::Value previousSourceRoot;
	std::string readError;
	REQUIRE(SceneSerializerStorage::ReadRootFromFile(prefabPath, previousSourceRoot, &readError));

	const auto countBySourceId = [&](uint64_t sourceId) {
		int count = 0;
		auto view = gameScene->GetRegistry().view<PrefabInstanceComponent>();
		for (entt::entity e : view) {
			if (view.get<PrefabInstanceComponent>(e).SourceEntityId == sourceId) {
				++count;
			}
		}
		return count;
	};

	const EntityHandle childRefreshResult =
		SceneSerializer::RefreshPrefabInstance(*gameScene, instanceChild, previousSourceRoot);
	CHECK(childRefreshResult == entt::null);            // skipped as a non-root
	REQUIRE(gameScene->IsValid(instanceRoot));          // root untouched
	REQUIRE(gameScene->IsValid(instanceChild));         // child not destroyed
	CHECK(gameScene->GetComponent<NameComponent>(instanceRoot).Name == "RootEntity");
	CHECK(gameScene->GetComponent<NameComponent>(instanceChild).Name == "ChildEntity");
	CHECK(countBySourceId(rootSourceId) == 1);          // no duplicate root
	CHECK(countBySourceId(childSourceId) == 1);

	const EntityHandle newRoot =
		SceneSerializer::RefreshPrefabInstance(*gameScene, instanceRoot, previousSourceRoot);
	REQUIRE(newRoot != entt::null);
	REQUIRE(gameScene->IsValid(newRoot));
	CHECK(gameScene->GetComponent<NameComponent>(newRoot).Name == "RootEntity");
	REQUIRE(gameScene->HasComponent<HierarchyComponent>(newRoot));
	REQUIRE(gameScene->GetComponent<HierarchyComponent>(newRoot).Children.size() == 1);
	const EntityHandle newChild =
		gameScene->GetComponent<HierarchyComponent>(newRoot).Children.front();
	CHECK(gameScene->GetComponent<NameComponent>(newChild).Name == "ChildEntity");
	CHECK(countBySourceId(rootSourceId) == 1);
	CHECK(countBySourceId(childSourceId) == 1);
}
