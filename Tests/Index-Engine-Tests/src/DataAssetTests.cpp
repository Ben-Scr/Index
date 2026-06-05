#include <doctest/doctest.h>

#include "Scripting/DataAssetManager.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializerShared.hpp"

#include <filesystem>

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
