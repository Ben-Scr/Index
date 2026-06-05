#include "pch.hpp"
#include "Scripting/DataAssetManager.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/SceneSerializerShared.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Core/Application.hpp"
#include "Core/Log.hpp"

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Index {

	namespace {
		constexpr int k_DataAssetVersion = 1;

		std::mutex s_Mutex;
		std::unordered_map<uint64_t, std::string> s_LoadedTypes; // guid -> C# type name

		using FieldList = std::vector<std::pair<std::string, std::string>>;

		// The editor writes in the project's "Asset format" setting; a standalone
		// runtime always writes binary. Reads auto-detect either way.
		SceneSerializationFormat ResolveFormat() {
			if (!Application::IsEditor())
				return SceneSerializationFormat::Binary;
			if (IndexProject* project = ProjectManager::GetCurrentProject())
				return project->AssetSerializationFormat == IndexProject::ProjectAssetSerializationFormat::Binary
					? SceneSerializationFormat::Binary
					: SceneSerializationFormat::Json;
			return SceneSerializationFormat::Json;
		}

		// Read + parse a .dataasset file into its C# type and name->value field map
		// (values in the scripting wire format). False if missing or malformed.
		bool ReadFile(uint64_t guid, std::string& outType, FieldList& outFields) {
			const std::string path = AssetRegistry::ResolvePath(guid);
			if (path.empty())
				return false;

			// ReadRootFromFile auto-detects JSON vs the binary container.
			std::string error;
			Json::Value root;
			if (!SceneSerializerStorage::ReadRootFromFile(path, root, &error) || !root.IsObject()) {
				IDX_CORE_ERROR_TAG("DataAsset", "Read failed for '{}': {}", path, error);
				return false;
			}

			if (const Json::Value* type = root.FindMember("type"))
				outType = type->AsStringOr("");
			if (outType.empty())
				return false;

			if (const Json::Value* fields = root.FindMember("fields"); fields && fields->IsObject()) {
				for (const auto& [name, value] : fields->GetObject())
					outFields.emplace_back(name, value.AsStringOr(""));
			}
			return true;
		}

		void MarkLoaded(uint64_t guid, const std::string& type) {
			std::lock_guard<std::mutex> lock(s_Mutex);
			s_LoadedTypes[guid] = type;
		}

		void ReplayFields(uint64_t guid, const FieldList& fields) {
			for (const auto& [name, value] : fields)
				ScriptEngine::SetDataAssetField(guid, name.c_str(), value.c_str());
		}

		// Create the managed instance and replay its fields. The guid is marked loaded
		// *before* the replay so a field that references this same asset (directly or
		// through a cycle) resolves to the live instance instead of recursing forever.
		bool Instantiate(uint64_t guid) {
			std::string type;
			FieldList fields;
			if (!ReadFile(guid, type, fields))
				return false;
			if (!ScriptEngine::DataAssetClassExists(type))
				return false; // type not available yet (no user assembly, or renamed/removed)
			if (ScriptEngine::CreateDataAssetInstance(type, guid) == 0)
				return false;

			MarkLoaded(guid, type);
			ReplayFields(guid, fields);
			return true;
		}
	}

	bool DataAssetManager::Load(uint64_t guid) {
		if (guid == 0)
			return false;
		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			if (s_LoadedTypes.find(guid) != s_LoadedTypes.end())
				return true;
		}
		return Instantiate(guid);
	}

	void DataAssetManager::Unload(uint64_t guid) {
		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			if (s_LoadedTypes.erase(guid) == 0)
				return;
		}
		ScriptEngine::DestroyDataAssetInstance(guid);
	}

	void DataAssetManager::DropAll() {
		std::lock_guard<std::mutex> lock(s_Mutex);
		s_LoadedTypes.clear();
	}

	void DataAssetManager::ReloadAll() {
		std::vector<uint64_t> guids;
		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			guids.reserve(s_LoadedTypes.size());
			for (const auto& [guid, type] : s_LoadedTypes)
				guids.push_back(guid);
		}

		struct Pending { uint64_t Guid; std::string Type; FieldList Fields; };
		std::vector<Pending> pending;
		pending.reserve(guids.size());

		for (uint64_t guid : guids) {
			Pending p;
			p.Guid = guid;
			if (ReadFile(guid, p.Type, p.Fields) && ScriptEngine::DataAssetClassExists(p.Type)) {
				pending.push_back(std::move(p));
			} else {
				std::lock_guard<std::mutex> lock(s_Mutex);
				s_LoadedTypes.erase(guid); // file or type gone — can't re-hydrate
			}
		}

		// Create every instance first so cross-asset references resolve during replay.
		for (const Pending& p : pending) {
			if (ScriptEngine::CreateDataAssetInstance(p.Type, p.Guid) != 0)
				MarkLoaded(p.Guid, p.Type);
		}
		for (const Pending& p : pending)
			ReplayFields(p.Guid, p.Fields);
	}

	const char* DataAssetManager::GetFields(uint64_t guid) {
		Load(guid);
		return ScriptEngine::GetDataAssetFields(guid);
	}

	void DataAssetManager::SetField(uint64_t guid, const std::string& fieldName, const std::string& value) {
		Load(guid);
		ScriptEngine::SetDataAssetField(guid, fieldName.c_str(), value.c_str());
	}

	std::string DataAssetManager::GetTypeName(uint64_t guid) {
		std::lock_guard<std::mutex> lock(s_Mutex);
		auto it = s_LoadedTypes.find(guid);
		return it != s_LoadedTypes.end() ? it->second : std::string();
	}

	bool DataAssetManager::Save(uint64_t guid) {
		const std::string path = AssetRegistry::ResolvePath(guid);
		if (path.empty() || !Load(guid))
			return false;

		// GetDataAssetFields returns the inspector field array [{name,type,value,...}];
		// persist only the name->value pairs (metadata comes from the class attributes).
		const char* fieldsJson = ScriptEngine::GetDataAssetFields(guid);
		Json::Value parsed = Json::Parse(fieldsJson ? fieldsJson : "[]");

		Json::Value root = Json::Value::MakeObject();
		root.AddMember("version", Json::Value(k_DataAssetVersion));
		root.AddMember("type", Json::Value(GetTypeName(guid)));

		Json::Value fields = Json::Value::MakeObject();
		if (parsed.IsArray()) {
			for (const Json::Value& field : parsed.GetArray()) {
				if (!field.IsObject())
					continue;
				const Json::Value* name = field.FindMember("name");
				const Json::Value* value = field.FindMember("value");
				if (name && value)
					fields.AddMember(name->AsStringOr(""), Json::Value(value->AsStringOr("")));
			}
		}
		root.AddMember("fields", std::move(fields));

		if (!SceneSerializerStorage::WriteRootToFile(path, root, ResolveFormat())) {
			IDX_CORE_ERROR_TAG("DataAsset", "Failed to write '{}'", path);
			return false;
		}
		return true;
	}

	uint64_t DataAssetManager::Create(const std::string& path, const std::string& typeName) {
		Json::Value root = Json::Value::MakeObject();
		root.AddMember("version", Json::Value(k_DataAssetVersion));
		root.AddMember("type", Json::Value(typeName));
		root.AddMember("fields", Json::Value::MakeObject());

		if (!SceneSerializerStorage::WriteRootToFile(path, root, ResolveFormat())) {
			IDX_CORE_ERROR_TAG("DataAsset", "Failed to write new data asset '{}'", path);
			return 0;
		}

		// Mints (and persists, via .meta) a stable GUID for the new file.
		return AssetRegistry::GetOrCreateAssetUUID(path);
	}

}
