#include <doctest/doctest.h>

#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/PrefabInstanceComponent.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/General/UUIDComponent.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializer.hpp"

using namespace Index;

TEST_CASE("Clipboard entity serialization preserves child hierarchy")
{
	auto scene = Scene::CreateDetachedScene("Clipboard Test");

	Entity root = scene->CreateEntity("Root");
	auto& rootTransform = root.GetComponent<Transform2DComponent>();
	rootTransform.LocalPosition = { 2.0f, 3.0f };
	rootTransform.Position = rootTransform.LocalPosition;

	Entity child = scene->CreateEntity("Child");
	auto& childTransform = child.GetComponent<Transform2DComponent>();
	childTransform.LocalPosition = { -5.0f, 0.0f };
	childTransform.Position = childTransform.LocalPosition;
	child.SetParent(root);

	const uint64_t originalRootId = static_cast<uint64_t>(scene->GetComponent<UUIDComponent>(root.GetHandle()).Id);
	const uint64_t originalChildId = static_cast<uint64_t>(scene->GetComponent<UUIDComponent>(child.GetHandle()).Id);

	Json::Value clipboardValue = SceneSerializer::SerializeEntityForClipboard(*scene, root.GetHandle());
	REQUIRE(clipboardValue.IsObject());

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, clipboardValue);
	REQUIRE(cloneHandle != entt::null);
	REQUIRE(scene->IsValid(cloneHandle));

	Entity clone = scene->GetEntity(cloneHandle);
	CHECK(clone.GetName() == "Root");
	CHECK(static_cast<uint64_t>(scene->GetComponent<UUIDComponent>(cloneHandle).Id) != originalRootId);

	const auto& clonedChildren = clone.GetChildren();
	REQUIRE(clonedChildren.size() == 1);

	Entity clonedChild = scene->GetEntity(clonedChildren.front());
	CHECK(clonedChild.GetName() == "Child");
	CHECK(clonedChild.GetParent().GetHandle() == cloneHandle);
	CHECK(static_cast<uint64_t>(scene->GetComponent<UUIDComponent>(clonedChild.GetHandle()).Id) != originalChildId);

	const auto& clonedChildTransform = clonedChild.GetComponent<Transform2DComponent>();
	CHECK(clonedChildTransform.LocalPosition.x == doctest::Approx(-5.0f));
	CHECK(clonedChildTransform.LocalPosition.y == doctest::Approx(0.0f));
}

TEST_CASE("Clipboard entity serialization preserves grandchild hierarchy")
{
	auto scene = Scene::CreateDetachedScene("Clipboard Deep Test");

	Entity root = scene->CreateEntity("Root");
	Entity child = scene->CreateEntity("Child");
	Entity grandchild = scene->CreateEntity("Grandchild");
	Entity grandchild2 = scene->CreateEntity("Grandchild2");
	child.SetParent(root);
	grandchild.SetParent(child);
	grandchild2.SetParent(child);

	Json::Value clipboardValue = SceneSerializer::SerializeEntityForClipboard(*scene, root.GetHandle());
	REQUIRE(clipboardValue.IsObject());

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, clipboardValue);
	REQUIRE(cloneHandle != entt::null);

	Entity clone = scene->GetEntity(cloneHandle);
	REQUIRE(clone.GetChildren().size() == 1);

	Entity clonedChild = scene->GetEntity(clone.GetChildren().front());
	CHECK(clonedChild.GetName() == "Child");
	REQUIRE(clonedChild.GetChildren().size() == 2);

	Entity clonedGrandchild = scene->GetEntity(clonedChild.GetChildren()[0]);
	Entity clonedGrandchild2 = scene->GetEntity(clonedChild.GetChildren()[1]);
	CHECK(clonedGrandchild.GetParent().GetHandle() == clonedChild.GetHandle());
	CHECK(clonedGrandchild2.GetParent().GetHandle() == clonedChild.GetHandle());
	CHECK((clonedGrandchild.GetName() == "Grandchild" || clonedGrandchild.GetName() == "Grandchild2"));
}

TEST_CASE("Editor-style duplicate (re-parenting under source's parent) preserves children")
{
	// Mimics ImGuiEditorLayer::DuplicateSelectedEntity: serialize for
	// clipboard, deserialize, then re-parent the new root under the
	// original's parent so the duplicate appears as a sibling. The bug
	// would surface here if SetParent on the clone's root inadvertently
	// dropped the clone's own children.
	auto scene = Scene::CreateDetachedScene("Editor Duplicate Test");

	Entity grandparent = scene->CreateEntity("Grandparent");
	Entity parent = scene->CreateEntity("Parent");
	Entity child = scene->CreateEntity("Child");
	parent.SetParent(grandparent);
	child.SetParent(parent);

	Json::Value clipboardValue = SceneSerializer::SerializeEntityForClipboard(*scene, parent.GetHandle());
	REQUIRE(clipboardValue.IsObject());

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, clipboardValue);
	REQUIRE(cloneHandle != entt::null);

	// Editor re-parents the clone under the source's parent so it shows
	// up as a sibling. After this, the clone's own children must remain
	// attached to the clone — not stripped or moved to grandparent.
	Entity clone = scene->GetEntity(cloneHandle);
	clone.SetParent(grandparent);

	CHECK(clone.GetParent().GetHandle() == grandparent.GetHandle());
	REQUIRE(clone.GetChildren().size() == 1);

	Entity clonedChild = scene->GetEntity(clone.GetChildren().front());
	CHECK(clonedChild.GetName() == "Child");
	CHECK(clonedChild.GetParent().GetHandle() == cloneHandle);
}

TEST_CASE("Duplicating a prefab instance preserves prefab origin and PrefabGUID")
{
	// Regression: editor-side Duplicate (Ctrl+D) routes through
	// SerializeEntityForClipboard + DeserializeEntityFromValue. The clipboard
	// path stripped Origin / PrefabGUID with the rest of the identity members,
	// so the duplicate came back as a plain Scene entity — no
	// PrefabInstanceComponent, no asset link, no override tracking.
	auto scene = Scene::CreateDetachedScene("Prefab Duplicate Test");

	Entity source = scene->CreateEntity("PrefabRoot");
	Entity sourceChild = scene->CreateEntity("PrefabChild");
	sourceChild.SetParent(source);

	constexpr uint64_t k_PrefabGuid = 0x1234ABCDull;
	constexpr uint64_t k_RootSourceEntityId = 0xAAAA1ull;
	constexpr uint64_t k_ChildSourceEntityId = 0xBBBB2ull;

	scene->SetEntityMetaData(source.GetHandle(), EntityOrigin::Prefab, AssetGUID(k_PrefabGuid));
	scene->SetEntityMetaData(sourceChild.GetHandle(), EntityOrigin::Prefab, AssetGUID(k_PrefabGuid));
	REQUIRE(scene->HasComponent<PrefabInstanceComponent>(source.GetHandle()));
	scene->GetComponent<PrefabInstanceComponent>(source.GetHandle()).SourceEntityId = k_RootSourceEntityId;
	scene->GetComponent<PrefabInstanceComponent>(sourceChild.GetHandle()).SourceEntityId = k_ChildSourceEntityId;

	Json::Value clipboardValue = SceneSerializer::SerializeEntityForClipboard(*scene, source.GetHandle());
	REQUIRE(clipboardValue.IsObject());

	const EntityHandle cloneHandle = SceneSerializer::DeserializeEntityFromValue(*scene, clipboardValue);
	REQUIRE(cloneHandle != entt::null);

	CHECK(scene->GetEntityOrigin(cloneHandle) == EntityOrigin::Prefab);
	CHECK(static_cast<uint64_t>(scene->GetPrefabGUID(cloneHandle)) == k_PrefabGuid);
	REQUIRE(scene->HasComponent<PrefabInstanceComponent>(cloneHandle));
	CHECK(scene->GetComponent<PrefabInstanceComponent>(cloneHandle).SourceEntityId == k_RootSourceEntityId);

	Entity clone = scene->GetEntity(cloneHandle);
	REQUIRE(clone.GetChildren().size() == 1);
	const EntityHandle cloneChildHandle = clone.GetChildren().front();
	CHECK(scene->GetEntityOrigin(cloneChildHandle) == EntityOrigin::Prefab);
	CHECK(static_cast<uint64_t>(scene->GetPrefabGUID(cloneChildHandle)) == k_PrefabGuid);
	REQUIRE(scene->HasComponent<PrefabInstanceComponent>(cloneChildHandle));
	CHECK(scene->GetComponent<PrefabInstanceComponent>(cloneChildHandle).SourceEntityId == k_ChildSourceEntityId);
}
