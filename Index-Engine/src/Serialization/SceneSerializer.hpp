#pragma once
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Core/Export.hpp"
#include "Scene/EntityHandle.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Index {

	enum class SceneSerializationFormat : std::uint8_t {
		Json = 0,
		Binary = 1
	};

	class Scene;

	class Entity;

	namespace Json {
		class Value;
	}

	class INDEX_API SceneSerializer {
	public:
		static Json::Value SerializeScene(Scene& scene);
		static bool DeserializeScene(Scene& scene, const Json::Value& root, std::string_view source = {});

		static bool SaveToFile(Scene& scene, const std::string& path);
		static bool SaveToFile(Scene& scene, const std::string& path, bool logSuccess);
		static bool SaveToFile(Scene& scene, const std::string& path, SceneSerializationFormat format);
		static bool SaveToFile(Scene& scene, const std::string& path, SceneSerializationFormat format, bool logSuccess);
		static bool LoadFromFile(Scene& scene, const std::string& path);

		// Prefab support: save/load single entities
		static Json::Value SerializeEntityFull(Scene& scene, EntityHandle entity);
		static Json::Value SerializeEntityForClipboard(Scene& scene, EntityHandle entity);
		static EntityHandle DeserializeEntityFromValue(Scene& scene, const Json::Value& entityValue);
		static Json::Value SerializeComponent(Scene& scene, EntityHandle entity, std::string_view componentName);
		static bool DeserializeComponent(Scene& scene, EntityHandle entity, std::string_view componentName, const Json::Value& componentValue);
		static bool ResetComponent(Scene& scene, EntityHandle entity, std::string_view componentName);
		static bool SaveEntityToFile(Scene& scene, EntityHandle entity, const std::string& path);
		static bool SaveEntityToFile(Scene& scene, EntityHandle entity, const std::string& path, SceneSerializationFormat format);
		// Saves entity subtree as a .prefab, converts the live subtree in-place into an instance (all entities get Origin::Prefab + PrefabInstanceComponent matching the written uuid). Returns GUID or 0 on failure.
		static uint64_t SaveEntityAsPrefabInstance(Scene& scene, EntityHandle entity, const std::string& path);
		static EntityHandle LoadEntityFromFile(Scene& scene, const std::string& path);
		static bool ConvertFileFormat(const std::string& path, SceneSerializationFormat format);
		static bool IsBinarySerializedFile(const std::string& path);
		static EntityHandle InstantiatePrefab(Scene& scene, uint64_t prefabGuid);
		static bool ApplyPrefabInstanceOverrides(Scene& scene, EntityHandle entity);
		static EntityHandle RevertPrefabInstanceOverride(Scene& scene, EntityHandle entity, const std::string& overridePath);

		// `previousSourceEntityValue` is the prefab "Entity" block BEFORE the source was saved; used as override baseline so per-field overrides survive a source edit.
		static EntityHandle RefreshPrefabInstance(Scene& scene, EntityHandle existing,
			const Json::Value& previousSourceEntityValue);

		// outOverrides is filled with dot-path keys e.g. "Transform2D.posX". Returns false for non-instances and orphans (source GUID not in AssetRegistry).
		static bool ComputeInstanceOverrides(Scene& scene, EntityHandle entity, Json::Value& outOverrides);

		// NOTE: PrefabInstanceComponent is on every entity in the subtree (not just the root), so the root is determined by walking HierarchyComponent.Parent upward.
		static EntityHandle GetPrefabInstanceRoot(Scene& scene, EntityHandle entity);

		// Walks the entire subtree so a child-only edit still enables Apply/Revert All on the root. `entity` may be any member of the instance.
		static bool HasPrefabInstanceOverrides(Scene& scene, EntityHandle entity);

	private:
		static EntityHandle DeserializeEntity(Scene& scene, const Json::Value& entityValue);
		static EntityHandle DeserializeFullEntity(
			Scene& scene,
			const Json::Value& entityValue,
			EntityOrigin origin,
			uint64_t prefabGuid = 0);
		static EntityHandle DeserializeEntityTree(
			Scene& scene,
			const std::vector<Json::Value>& entityValues,
			EntityOrigin origin,
			uint64_t prefabGuid = 0,
			bool preserveSerializedIdentity = true);
		static EntityHandle DeserializePrefabInstance(Scene& scene, const Json::Value& entityValue);
	};

} // namespace Index
