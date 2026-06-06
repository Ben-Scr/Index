#include <doctest/doctest.h>

#include "Assets/AssetRegistry.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scripting/DataAssetManager.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializerShared.hpp"

#include <chrono>
#include <filesystem>
#include <memory>

using namespace Index;

// Locks the on-disk .dataasset schema that DataAssetManager::Create writes.
// The file is written before GUID registration, so this is independent of a
// loaded project or the scripting host.
TEST_CASE("DataAssetManager::Create writes a versioned, typed .dataasset stub") {
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "IndexDataAssetTest";
	fs::create_directories(dir);
	const std::string path = (dir / "Sword.dataasset").string();
	fs::remove(path);

	// guid may be 0 here (the temp dir isn't a tracked project), but the stub
	// file is written regardless — that's the part under test.
	DataAssetManager::Create(path, "ItemData");

	REQUIRE(File::Exists(path));

	// Read back format-agnostically: this is a non-editor process, so Create
	// writes the binary container; ReadRootFromFile auto-detects it.
	Json::Value root;
	REQUIRE(SceneSerializerStorage::ReadRootFromFile(path, root));
	REQUIRE(root.IsObject());

	const Json::Value* type = root.FindMember("type");
	REQUIRE(type != nullptr);
	CHECK(type->AsStringOr() == "ItemData");

	const Json::Value* version = root.FindMember("version");
	REQUIRE(version != nullptr);
	CHECK(version->AsIntOr(0) == 1);

	const Json::Value* fields = root.FindMember("fields");
	REQUIRE(fields != nullptr);
	CHECK(fields->IsObject());

	fs::remove(path);
}

// The Load/Save/Unload/GetTypeName surface fans out to the managed scripting
// host, which the test harness doesn't boot. These cases stay on the paths that
// return *before* any host call — the guid==0 guard, an unresolvable GUID, and
// the empty loaded-set — so they verify the contract the engine relies on when
// no asset is live (and that none of them crash without a host).
TEST_CASE("DataAssetManager state queries are host-independent and safe") {
	const uint64_t unknown = 0xDA7A55E7ull; // not registered in any project

	CHECK_FALSE(DataAssetManager::Load(0));        // guid 0 rejected up front
	CHECK_FALSE(DataAssetManager::Load(unknown));  // no file resolves -> false
	CHECK(DataAssetManager::GetTypeName(unknown).empty());
	CHECK_FALSE(DataAssetManager::Save(unknown));  // unresolvable path -> false

	CHECK_NOTHROW(DataAssetManager::Unload(unknown)); // not loaded -> no-op
	CHECK_NOTHROW(DataAssetManager::DropAll());
	CHECK(DataAssetManager::GetTypeName(unknown).empty());
}

// Create writes the stub (covered above) and then mints a GUID via the asset
// registry. A GUID is only minted for a project-tracked path, so this needs a
// current project — without one Create still writes the file but returns 0.
TEST_CASE("DataAssetManager::Create registers a stable GUID for a project-tracked file") {
	namespace fs = std::filesystem;
	const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path projectRoot = fs::temp_directory_path() / ("IndexDataAssetCreate_" + std::to_string(suffix));
	const fs::path assets = projectRoot / "Assets";

	struct Cleanup {
		fs::path Root;
		~Cleanup() {
			ProjectManager::SetCurrentProject(std::unique_ptr<IndexProject>());
			std::error_code ec;
			fs::remove_all(Root, ec);
		}
	} cleanup{ projectRoot };

	fs::create_directories(assets);

	auto project = std::make_unique<IndexProject>();
	project->Name = "DataAsset Create Test";
	project->RootDirectory = projectRoot.string();
	project->AssetsDirectory = assets.string();
	ProjectManager::SetCurrentProject(std::move(project));

	const std::string path = (assets / "Goblin.dataasset").string();
	const uint64_t guid = DataAssetManager::Create(path, "EnemyData");

	REQUIRE(File::Exists(path));
	CHECK(guid != 0);                                          // tracked file -> real GUID
	CHECK(AssetRegistry::GetOrCreateAssetUUID(path) == guid);  // stable on re-query

	Json::Value root;
	REQUIRE(SceneSerializerStorage::ReadRootFromFile(path, root));
	const Json::Value* type = root.FindMember("type");
	REQUIRE(type != nullptr);
	CHECK(type->AsStringOr() == "EnemyData");
}
