#include <doctest/doctest.h>

#include "Components/General/UUIDComponent.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializer.hpp"

#include <filesystem>
#include <string>

using namespace Index;

// Companion to SceneSerializerClipboardTests' entity-ref remap coverage, but for prefab instantiation:
// a prefab whose template entity references one of its OWN child entities must, when instantiated,
// resolve that ref to THIS instance's child — not to null and not to a foreign entity that happens to
// share the template's source id. The remap is keyed on each template entity's own "uuid".

namespace {
	Json::Value MakePrefabEntity(const char* uuid, const char* name, const char* parentUuid) {
		using Json::Value;
		Value e = Value::MakeObject();
		e.AddMember("uuid", Value(std::string(uuid)));
		e.AddMember("name", Value(std::string(name)));
		Value t = Value::MakeObject();
		t.AddMember("posX", Value(0.0f));
		t.AddMember("posY", Value(0.0f));
		t.AddMember("rotation", Value(0.0f));
		t.AddMember("scaleX", Value(1.0f));
		t.AddMember("scaleY", Value(1.0f));
		e.AddMember("Transform2D", std::move(t));
		if (parentUuid) {
			e.AddMember("parentUuid", Value(std::string(parentUuid)));
		}
		return e;
	}
}

TEST_CASE("Prefab instantiation remaps a child-to-child entity ref to the instance's own child")
{
	using Json::Value;

	// Template ids (the "uuid" members the prefab's stored refs are written against).
	constexpr const char* k_RootUuid   = "1001";
	constexpr const char* k_ChildAUuid = "2002";
	constexpr const char* k_ChildBUuid = "3003";

	// Root container; ChildA's script field references its sibling ChildB by ChildB's template uuid.
	Value root = MakePrefabEntity(k_RootUuid, "PrefabRoot", nullptr);

	Value childA = MakePrefabEntity(k_ChildAUuid, "ChildA", k_RootUuid);
	{
		Value scripts = Value::MakeArray();
		Value s = Value::MakeObject();
		s.AddMember("className", Value(std::string("TestScript")));
		s.AddMember("type", Value(std::string("Managed")));
		scripts.Append(std::move(s));
		childA.AddMember("Scripts", std::move(scripts));

		Value fields = Value::MakeArray();
		Value target = Value::MakeObject();
		target.AddMember("name", Value(std::string("Target")));
		target.AddMember("type", Value(std::string("entity")));
		target.AddMember("value", Value(std::string(k_ChildBUuid)));  // points at the sibling
		fields.Append(std::move(target));

		Value fieldsByClass = Value::MakeObject();
		fieldsByClass.AddMember("TestScript", std::move(fields));
		childA.AddMember("ScriptFields", std::move(fieldsByClass));
	}

	Value childB = MakePrefabEntity(k_ChildBUuid, "ChildB", k_RootUuid);

	Value entities = Value::MakeArray();
	entities.Append(std::move(root));
	entities.Append(std::move(childA));
	entities.Append(std::move(childB));

	Value prefabRoot = Value::MakeObject();
	prefabRoot.AddMember("Entities", std::move(entities));

	// Write the template to a temp .prefab file and load it via LoadEntityFromFile, which instantiates the
	// prefab subtree. (InstantiatePrefab needs the GUID registered in AssetRegistry, whose state lives in
	// the engine DLL and isn't reachable from the test EXE; loading a loose file routes entirely DLL-side.)
	const std::filesystem::path filePath =
		std::filesystem::temp_directory_path() / "IndexPrefabChildRefTest.prefab";
	const std::string filePathStr = filePath.string();
	REQUIRE(File::WriteAllText(filePathStr, Json::Stringify(prefabRoot, true)));

	auto scene = Scene::CreateDetachedScene("Prefab Child Ref Test");

	const EntityHandle instanceRoot = SceneSerializer::LoadEntityFromFile(*scene, filePathStr);
	REQUIRE(instanceRoot != entt::null);
	REQUIRE(scene->IsValid(instanceRoot));

	Entity rootEntity = scene->GetEntity(instanceRoot);
	const auto& children = rootEntity.GetChildren();
	REQUIRE(children.size() == 2);

	EntityHandle childAHandle = entt::null;
	EntityHandle childBHandle = entt::null;
	for (EntityHandle child : children) {
		const std::string name = scene->GetEntity(child).GetName();
		if (name == "ChildA") childAHandle = child;
		else if (name == "ChildB") childBHandle = child;
	}
	REQUIRE(childAHandle != entt::null);
	REQUIRE(childBHandle != entt::null);

	const uint64_t childBId = scene->GetEntityPersistentID(childBHandle);
	REQUIRE(childBId != 0);
	// Sanity: the instance's child got a fresh persistent id, not the template's source id.
	REQUIRE(childBId != 3003ull);

	REQUIRE(scene->HasComponent<ScriptComponent>(childAHandle));
	const auto& scriptComponent = scene->GetComponent<ScriptComponent>(childAHandle);
	const auto fieldIt = scriptComponent.PendingFieldValues.find("TestScript.Target");
	REQUIRE(fieldIt != scriptComponent.PendingFieldValues.end());

	// The ref was stored against ChildB's template uuid ("3003"); after instantiation it must resolve to
	// THIS instance's ChildB, i.e. its fresh persistent id — not the un-remapped template id.
	CHECK(fieldIt->second == std::to_string(childBId));
	CHECK(fieldIt->second != std::string(k_ChildBUuid));

	std::error_code removeError;
	std::filesystem::remove(filePath, removeError);
}
