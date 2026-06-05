#pragma once

#include "Assets/AssetKind.hpp"
#include "Assets/TextureMeta.hpp"
#include "Core/UUID.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/Path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <magic_enum/magic_enum.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Index {

	class AssetRegistry {
	public:
		struct Record {
			uint64_t Id = 0;
			std::string Path;
			AssetKind Kind = AssetKind::Unknown;
		};

		static constexpr std::string_view MetaExtension = ".meta";

		static bool IsMetaFilePath(std::string_view path) {
			const std::string normalized = ToLowerCopy(std::string(path));
			const std::string metaExtension(MetaExtension);
			if (normalized.size() < metaExtension.size()) {
				return false;
			}

			return normalized.compare(normalized.size() - metaExtension.size(), metaExtension.size(), metaExtension) == 0;
		}

		static void MarkDirty() {
			s_Dirty = true;
			// Clear the known-missing cache: a FileWatcher event (or any
			// other dirtying signal) means the on-disk layout might have
			// changed and previously-missing assets could now exist.
			s_KnownMissingIds.clear();
		}

		// Top-byte convention: 0xAB = hand-picked stable GUID (safe in defaults), 0xAA = path-hash GUID (changes on rename/move within IndexAssets/).
		static void RegisterBuiltInAsset(const std::string& absolutePath, uint64_t guid, AssetKind kind) {
			if (absolutePath.empty() || guid == 0) {
				return;
			}
			Record record;
			record.Id = guid;
			record.Path = absolutePath;
			record.Kind = kind;
			s_BuiltInById[guid] = record;
			s_BuiltInPathToId[absolutePath] = guid;
		}

		// Call AFTER RegisterBuiltInAsset so hand-picked GUIDs win over path-hash auto-registration.
		static void RegisterBuiltInDirectory(const std::string& absoluteRoot) {
			if (absoluteRoot.empty()) {
				return;
			}
			std::error_code rootEc;
			if (!std::filesystem::is_directory(absoluteRoot, rootEc) || rootEc) {
				return;
			}

			std::error_code ec;
			for (std::filesystem::recursive_directory_iterator it(
				 absoluteRoot,
				 std::filesystem::directory_options::skip_permission_denied,
				 ec), end;
				 it != end;
				 it.increment(ec))
			{
				if (ec) { ec.clear(); continue; }
				if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }

				const std::string absPath = NormalizePath(it->path().string());
				if (absPath.empty() || IsMetaFilePath(absPath)) {
					continue;
				}
				if (s_BuiltInPathToId.find(absPath) != s_BuiltInPathToId.end()) {
					continue;  // hand-picked or earlier scan already owns this path
				}

				const AssetKind kind = Classify(absPath);
				if (kind == AssetKind::Unknown) {
					continue;
				}

				// FNV-1a 64 of the path relative to IndexAssets root, lowercased + forward-slashed for cross-platform stability.
				std::error_code relEc;
				std::filesystem::path relPath = std::filesystem::relative(
					std::filesystem::path(absPath),
					std::filesystem::path(absoluteRoot), relEc);
				std::string relStr = relEc ? absPath : relPath.generic_string();
				std::transform(relStr.begin(), relStr.end(), relStr.begin(),
					[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

				uint64_t hash = 0xcbf29ce484222325ULL;
				for (char ch : relStr) {
					hash ^= static_cast<uint8_t>(ch);
					hash *= 0x100000001b3ULL;
				}
				uint64_t guid = (hash & 0x00FFFFFFFFFFFFFFULL) | 0xAA00000000000000ULL;

				if (s_BuiltInById.find(guid) != s_BuiltInById.end()) {
					IDX_CORE_WARN_TAG("AssetRegistry",
						"Built-in path-hash collision: '{}' hashed to GUID {} which is already registered — skipping",
						absPath, guid);
					continue;
				}

				Record record;
				record.Id = guid;
				record.Path = absPath;
				record.Kind = kind;
				s_BuiltInById[guid] = record;
				s_BuiltInPathToId[absPath] = guid;
			}
		}

		static void Sync() {
			EnsureUpToDate();
		}

		static uint64_t GetOrCreateAssetUUID(const std::string& path) {
			const std::string normalizedPath = NormalizePath(path);
			if (normalizedPath.empty() || IsMetaFilePath(normalizedPath)) {
				return 0;
			}

			// Built-in (engine-shipped) assets bypass the project-tracked
			// path check — they live under <exeDir>/IndexAssets/ and have
			// pre-assigned stable GUIDs.
			if (auto it = s_BuiltInPathToId.find(normalizedPath); it != s_BuiltInPathToId.end()) {
				return it->second;
			}

			EnsureUpToDate();
			if (!IsTrackedAsset(normalizedPath)) {
				return 0;
			}

			if (const auto it = s_PathToId.find(normalizedPath); it != s_PathToId.end()) {
				return it->second;
			}

			uint64_t id = ReadMetaId(normalizedPath);
			if (id == 0) {
				do {
					id = static_cast<uint64_t>(UUID());
				} while (id != 0 && s_IdToRecord.find(id) != s_IdToRecord.end());
			} else if (auto existing = s_IdToRecord.find(id); existing != s_IdToRecord.end()) {
				// GUID collision: a different asset already owns this GUID.
				// Regenerate so we don't break the existing reference, but
				// surface the collision — silent overwrite breaks references
				// in scene/prefab files that point at the original asset.
				IDX_CORE_WARN_TAG("AssetRegistry",
					"GUID collision while indexing '{}': id {} already owned by '{}' — regenerating new GUID for '{}'",
					normalizedPath, id, existing->second.Path, normalizedPath);
				do {
					id = static_cast<uint64_t>(UUID());
				} while (id != 0 && s_IdToRecord.find(id) != s_IdToRecord.end());
			}

			WriteMeta(normalizedPath, id, Classify(normalizedPath));
			Register(normalizedPath, id, Classify(normalizedPath));
			s_KnownMissingIds.erase(id);
			return id;
		}

		static uint64_t GetGUID(const std::string& path) {
			return GetOrCreateAssetUUID(path);
		}

		static std::string ResolvePath(uint64_t assetId) {
			// Built-ins skip dirty/rebuild — their paths don't change at runtime.
			if (auto it = s_BuiltInById.find(assetId); it != s_BuiltInById.end()) {
				if (std::filesystem::exists(it->second.Path)) {
					return it->second.Path;
				}
				return {};
			}

			EnsureUpToDate();
			auto it = s_IdToRecord.find(assetId);
			if (it == s_IdToRecord.end()) {
				// Another editor/runtime module may have created the asset and
				// its .meta file after this registry copy last synced. Force one
				// recovery scan before treating the GUID as missing.
				if (s_KnownMissingIds.contains(assetId)) {
					return {};
				}

				s_Dirty = true;
				EnsureUpToDate();
				it = s_IdToRecord.find(assetId);
				if (it == s_IdToRecord.end()) {
					s_KnownMissingIds.insert(assetId);
					return {};
				}
			}

			if (std::filesystem::exists(it->second.Path)) {
				return it->second.Path;
			}

			// Cache miss so a permanently-moved asset doesn't trigger an O(N) rescan every frame; cleared by MarkDirty().
			if (s_KnownMissingIds.contains(assetId)) {
				return {};
			}

			s_Dirty = true;
			EnsureUpToDate();
			it = s_IdToRecord.find(assetId);
			if (it == s_IdToRecord.end() || !std::filesystem::exists(it->second.Path)) {
				s_KnownMissingIds.insert(assetId);
				return {};
			}

			return it->second.Path;
		}

		static AssetKind GetKind(uint64_t assetId) {
			if (auto it = s_BuiltInById.find(assetId); it != s_BuiltInById.end()) {
				return it->second.Kind;
			}
			EnsureUpToDate();
			auto it = s_IdToRecord.find(assetId);
			if (it == s_IdToRecord.end() && !s_KnownMissingIds.contains(assetId)) {
				// Match ResolvePath's recovery scan for late-created assets.
				s_Dirty = true;
				EnsureUpToDate();
				it = s_IdToRecord.find(assetId);
				if (it == s_IdToRecord.end()) {
					s_KnownMissingIds.insert(assetId);
				}
			}
			return it != s_IdToRecord.end() ? it->second.Kind : AssetKind::Unknown;
		}

		static bool Exists(uint64_t assetId) {
			return !ResolvePath(assetId).empty();
		}

		static bool IsBuiltIn(uint64_t assetId) {
			return s_BuiltInById.find(assetId) != s_BuiltInById.end();
		}

		static size_t GetBuiltInCount() {
			return s_BuiltInById.size();
		}

		static bool IsTexture(uint64_t assetId) {
			return GetKind(assetId) == AssetKind::Texture;
		}

		static bool IsAudio(uint64_t assetId) {
			return GetKind(assetId) == AssetKind::Audio;
		}

		static bool IsFont(uint64_t assetId) {
			return GetKind(assetId) == AssetKind::Font;
		}

		static std::vector<Record> GetAssetsByKind(AssetKind kind) {
			EnsureUpToDate();

			std::vector<Record> records;
			records.reserve(s_IdToRecord.size() + s_BuiltInById.size());

			for (const auto& [id, record] : s_IdToRecord) {
				if (kind != AssetKind::Unknown && record.Kind != kind) {
					continue;
				}
				records.push_back(record);
			}
			// Built-ins surface alongside project assets so the picker
			// shows the engine's default font/etc. as selectable entries.
			for (const auto& [id, record] : s_BuiltInById) {
				if (kind != AssetKind::Unknown && record.Kind != kind) {
					continue;
				}
				records.push_back(record);
			}

			std::sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
				const std::string aName = std::filesystem::path(a.Path).filename().string();
				const std::string bName = std::filesystem::path(b.Path).filename().string();
				if (aName == bName) {
					return a.Path < b.Path;
				}

				return aName < bName;
			});

			return records;
		}

		static std::vector<Record> FindAll(AssetKind kind, const std::string& pathPrefix = {}) {
			EnsureUpToDate();

			std::string normalizedPrefix;
			if (!pathPrefix.empty()) {
				std::filesystem::path prefixPath(pathPrefix);
				if (!prefixPath.is_absolute()) {
					const std::string root = GetAssetsRoot();
					if (!root.empty()) {
						prefixPath = std::filesystem::path(root) / prefixPath;
					}
				}
				normalizedPrefix = NormalizePath(prefixPath.string());
			}

			std::vector<Record> records;
			for (const auto& [id, record] : s_IdToRecord) {
				(void)id;
				if (kind != AssetKind::Unknown && record.Kind != kind) {
					continue;
				}
				if (!normalizedPrefix.empty()
					&& (record.Path.size() < normalizedPrefix.size()
						|| record.Path.compare(0, normalizedPrefix.size(), normalizedPrefix) != 0)) {
					continue;
				}
				records.push_back(record);
			}

			std::sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
				return a.Path < b.Path;
			});
			return records;
		}

		static std::string GetDisplayName(uint64_t assetId) {
			const std::string path = ResolvePath(assetId);
			if (path.empty()) {
				return {};
			}

			return std::filesystem::path(path).filename().string();
		}

		static std::string GetMetaPath(const std::string& assetPath) {
			if (assetPath.empty()) {
				return {};
			}

			return assetPath + std::string(MetaExtension);
		}

		static void DeleteCompanionMetadata(const std::string& assetPath) {
			const std::string normalizedPath = NormalizePath(assetPath);
			if (normalizedPath.empty() || IsMetaFilePath(normalizedPath)) {
				return;
			}

			const std::string metaPath = GetMetaPath(normalizedPath);
			std::error_code ec;
			std::filesystem::remove(metaPath, ec);
			MarkDirty();
		}

		static void MoveCompanionMetadata(const std::string& from, const std::string& to) {
			const std::string normalizedFrom = NormalizePath(from);
			const std::string normalizedTo = NormalizePath(to);
			if (normalizedFrom.empty() || normalizedTo.empty() || IsMetaFilePath(normalizedFrom) || IsMetaFilePath(normalizedTo)) {
				return;
			}

			const std::string metaFrom = GetMetaPath(normalizedFrom);
			if (!File::Exists(metaFrom)) {
				MarkDirty();
				return;
			}

			const std::string metaTo = GetMetaPath(normalizedTo);
			std::error_code ec;
			const std::filesystem::path target(metaTo);
			if (target.has_parent_path()) {
				std::filesystem::create_directories(target.parent_path(), ec);
				ec.clear();
			}

			std::filesystem::rename(metaFrom, metaTo, ec);
			if (ec) {
				ec.clear();
				std::filesystem::copy_file(metaFrom, metaTo, std::filesystem::copy_options::overwrite_existing, ec);
				if (!ec) {
					ec.clear();
					std::filesystem::remove(metaFrom, ec);
				}
			}

			MarkDirty();
		}

	private:
		static std::string GetAssetsRoot() {
			if (IndexProject* project = ProjectManager::GetCurrentProject()) {
				return NormalizePath(project->AssetsDirectory);
			}

			const std::string fallback = Path::Combine(Path::ExecutableDir(), "Assets");
			if (!std::filesystem::exists(fallback)) {
				return {};
			}

			return NormalizePath(fallback);
		}

		static void EnsureUpToDate() {
			const std::string root = GetAssetsRoot();
			if (root != s_TrackedRoot) {
				s_TrackedRoot = root;
				s_Dirty = true;
			}

			if (!s_Dirty) {
				return;
			}

			Rebuild();
		}

		static void Rebuild() {
			s_IdToRecord.clear();
			s_PathToId.clear();
			// New scan: previously-missing GUIDs may exist again.
			s_KnownMissingIds.clear();

			if (s_TrackedRoot.empty() || !std::filesystem::exists(s_TrackedRoot)) {
				s_Dirty = false;
				return;
			}

			std::error_code ec;
			for (std::filesystem::recursive_directory_iterator it(
				 s_TrackedRoot,
				 std::filesystem::directory_options::skip_permission_denied,
				 ec), end;
				 it != end;
				 it.increment(ec)) {
				if (ec) {
					ec.clear();
					continue;
				}

				if (!it->is_regular_file(ec) || ec) {
					ec.clear();
					continue;
				}

				const std::string assetPath = NormalizePath(it->path().string());
				if (assetPath.empty() || IsMetaFilePath(assetPath)) {
					continue;
				}

				IndexAsset(assetPath);
			}

			s_Dirty = false;
		}

		static void IndexAsset(const std::string& assetPath) {
			if (!IsTrackedAsset(assetPath)) {
				return;
			}

			AssetKind kind = Classify(assetPath);
			uint64_t id = ReadMetaId(assetPath);
			if (id == 0) {
				// Brand-new asset: mint an id and write its meta.
				id = static_cast<uint64_t>(UUID());
				WriteMeta(assetPath, id, kind);
			} else if (auto existing = s_IdToRecord.find(id); existing != s_IdToRecord.end()) {
				// Two assets claim the same GUID. Surface the collision so
				// the user knows references to one of them will silently
				// drift; the later-indexed asset gets a fresh GUID.
				IDX_CORE_WARN_TAG("AssetRegistry",
					"GUID collision while indexing '{}': id {} already owned by '{}' — regenerating new GUID for '{}'",
					assetPath, id, existing->second.Path, assetPath);
				id = static_cast<uint64_t>(UUID());
				WriteMeta(assetPath, id, kind);
			}
			Register(assetPath, id, kind);
		}

		static void Register(const std::string& assetPath, uint64_t id, AssetKind kind) {
			Record record;
			record.Id = id;
			record.Path = assetPath;
			record.Kind = kind;

			s_PathToId[assetPath] = id;
			s_IdToRecord[id] = std::move(record);
		}

		static std::string NormalizePath(const std::string& path) {
			if (path.empty()) {
				return {};
			}

			std::filesystem::path fsPath(path);
			if (!fsPath.is_absolute()) {
				if (IndexProject* project = ProjectManager::GetCurrentProject()) {
					const std::filesystem::path candidate = std::filesystem::path(project->AssetsDirectory) / fsPath;
					if (std::filesystem::exists(candidate)) {
						fsPath = candidate;
					}
					else {
						fsPath = std::filesystem::absolute(fsPath);
					}
				}
				else {
					const std::filesystem::path fallback = std::filesystem::path(Path::ExecutableDir()) / "Assets" / fsPath;
					if (std::filesystem::exists(fallback)) {
						fsPath = fallback;
					}
					else {
						fsPath = std::filesystem::absolute(fsPath);
					}
				}
			}

			std::error_code ec;
			std::filesystem::path normalized = std::filesystem::weakly_canonical(fsPath, ec);
			if (ec) {
				normalized = fsPath.lexically_normal();
			}

			return normalized.make_preferred().string();
		}

		static bool IsTrackedAsset(const std::string& assetPath) {
			if (assetPath.empty() || s_TrackedRoot.empty()) {
				return false;
			}

			if (!std::filesystem::exists(assetPath) || !std::filesystem::is_regular_file(assetPath)) {
				return false;
			}

			if (assetPath.size() < s_TrackedRoot.size() || assetPath.compare(0, s_TrackedRoot.size(), s_TrackedRoot) != 0) {
				return false;
			}

			if (assetPath.size() == s_TrackedRoot.size()) {
				return false;
			}

			const char separator = assetPath[s_TrackedRoot.size()];
			return separator == '\\' || separator == '/';
		}

		static AssetKind Classify(const std::string& assetPath) {
			const std::string extension = ToLowerCopy(std::filesystem::path(assetPath).extension().string());
			if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".tga") {
				return AssetKind::Texture;
			}
			if (extension == ".wav" || extension == ".mp3" || extension == ".ogg" || extension == ".flac") {
				return AssetKind::Audio;
			}
			if (extension == ".scene") {
				return AssetKind::Scene;
			}
			if (extension == ".prefab") {
				return AssetKind::Prefab;
			}
			if (extension == ".cs" || extension == ".cpp" || extension == ".c" || extension == ".hpp" || extension == ".h") {
				return AssetKind::Script;
			}
			if (extension == ".ttf" || extension == ".otf") {
				return AssetKind::Font;
			}
			if (extension == ".wgsl" || extension == ".vs" || extension == ".fs" || extension == ".vert" || extension == ".frag" || extension == ".glsl") {
				return AssetKind::Shader;
			}
			if (extension == ".dataasset") {
				return AssetKind::DataAsset;
			}
			if (!extension.empty()) {
				return AssetKind::Other;
			}

			return AssetKind::Unknown;
		}

		static uint64_t ReadMetaId(const std::string& assetPath) {
			const std::string metaPath = GetMetaPath(assetPath);

			// Skip the file read + JSON parse when (mtime, size) match a prior call — avoids ~1000 parses per dirty cycle on large projects.
			std::error_code ec;
			std::filesystem::file_time_type mtime{};
			std::uintmax_t size = 0;
			bool fingerprintOk = false;
			if (std::filesystem::exists(metaPath, ec) && !ec) {
				mtime = std::filesystem::last_write_time(metaPath, ec);
				if (!ec) {
					size = std::filesystem::file_size(metaPath, ec);
					fingerprintOk = !ec;
				}
			}
			if (!fingerprintOk) {
			}
			else {
				const auto cacheIt = s_MetaIdCache.find(metaPath);
				if (cacheIt != s_MetaIdCache.end()
					&& cacheIt->second.Mtime == mtime
					&& cacheIt->second.Size == size)
				{
					return cacheIt->second.AssetGuid;
				}
			}

			if (!File::Exists(metaPath)) {
				if (fingerprintOk) {
					s_MetaIdCache.erase(metaPath);
				}
				return 0;
			}

			Json::Value meta;
			std::string parseError;
			if (!Json::TryParse(File::ReadAllText(metaPath), meta, &parseError) || !meta.IsObject()) {
				return 0;
			}

			const Json::Value* uuidValue = meta.FindMember("AssetGUID");
			if (!uuidValue) {
				uuidValue = meta.FindMember("uuid");
			}
			if (!uuidValue) {
				return 0;
			}

			uint64_t parsed = 0;
			if (uuidValue->IsString()) {
				try {
					parsed = static_cast<uint64_t>(std::stoull(uuidValue->AsStringOr()));
				}
				catch (...) {
					parsed = 0;
				}
			}
			else {
				parsed = uuidValue->AsUInt64Or(0);
			}

			if (fingerprintOk && parsed != 0) {
				s_MetaIdCache[metaPath] = MetaIdCacheEntry{ mtime, size, parsed };
			}
			return parsed;
		}

		// Insert OR overwrite; AddMember alone is append-only and would silently produce duplicate keys.
		static void SetOrAddMember(Json::Value& object, const std::string& key, Json::Value value) {
			if (Json::Value* existing = object.FindMember(key)) {
				*existing = std::move(value);
				return;
			}
			object.AddMember(key, std::move(value));
		}

		static Json::Value LoadMetaJson(const std::string& metaPath) {
			Json::Value root = Json::Value::MakeObject();
			if (!File::Exists(metaPath)) {
				return root;
			}
			Json::Value parsed;
			std::string parseError;
			if (!Json::TryParse(File::ReadAllText(metaPath), parsed, &parseError) || !parsed.IsObject()) {
				return root;
			}
			return parsed;
		}

		// Merge-write: load existing meta, overwrite identity fields only — leaves `import`, `sprites`, etc. intact.
		static void WriteMeta(const std::string& assetPath, uint64_t id, AssetKind kind) {
			const std::string metaPath = GetMetaPath(assetPath);
			Json::Value meta = LoadMetaJson(metaPath);
			if (!meta.IsObject()) {
				meta = Json::Value::MakeObject();
			}
			SetOrAddMember(meta, "AssetGUID", Json::Value(std::to_string(id)));
			SetOrAddMember(meta, "uuid",      Json::Value(std::to_string(id)));
			SetOrAddMember(meta, "kind",      Json::Value(ToString(kind)));
			(void)File::WriteAllText(metaPath, Json::Stringify(meta, true));
		}

	public:
		static TextureMeta ReadTextureMeta(const std::string& assetPath) {
			TextureMeta meta;
			const std::string metaPath = GetMetaPath(assetPath);
			if (!File::Exists(metaPath)) {
				return meta;
			}
			Json::Value root;
			std::string parseError;
			if (!Json::TryParse(File::ReadAllText(metaPath), root, &parseError) || !root.IsObject()) {
				return meta;
			}

			if (const Json::Value* importNode = root.FindMember("import"); importNode && importNode->IsObject()) {
				meta.HasImportBlock = true;
				if (const Json::Value* filterNode = importNode->FindMember("filter")) {
					if (filterNode->IsString()) {
						if (auto parsed = magic_enum::enum_cast<Filter>(filterNode->AsStringOr()); parsed.has_value()) {
							meta.Import.FilterMode = parsed.value();
						}
					}
					else if (filterNode->IsNumber()) {
						meta.Import.FilterMode = static_cast<Filter>(filterNode->AsIntOr(static_cast<int>(meta.Import.FilterMode)));
					}
				}
				auto readWrap = [&](const char* key, Wrap& outWrap) {
					if (const Json::Value* node = importNode->FindMember(key)) {
						if (node->IsString()) {
							if (auto parsed = magic_enum::enum_cast<Wrap>(node->AsStringOr()); parsed.has_value()) {
								outWrap = parsed.value();
							}
						}
						else if (node->IsNumber()) {
							outWrap = static_cast<Wrap>(node->AsInt64Or(static_cast<int64_t>(outWrap)));
						}
					}
				};
				readWrap("wrapU", meta.Import.WrapU);
				readWrap("wrapV", meta.Import.WrapV);
			}

			if (const Json::Value* spritesNode = root.FindMember("sprites"); spritesNode && spritesNode->IsArray()) {
				for (const Json::Value& spriteValue : spritesNode->GetArray()) {
					if (!spriteValue.IsObject()) continue;
					SpriteSlice slice;
					if (const Json::Value* n = spriteValue.FindMember("name"); n && n->IsString()) {
						slice.Name = n->AsStringOr();
					}
					if (slice.Name.empty()) continue;
					if (const Json::Value* r = spriteValue.FindMember("rect"); r && r->IsArray() && r->GetArray().size() >= 4) {
						const auto& a = r->GetArray();
						slice.X = a[0].AsIntOr(0);
						slice.Y = a[1].AsIntOr(0);
						slice.W = a[2].AsIntOr(0);
						slice.H = a[3].AsIntOr(0);
					}
					if (slice.W <= 0 || slice.H <= 0) continue;
					if (const Json::Value* p = spriteValue.FindMember("pivot"); p && p->IsArray() && p->GetArray().size() >= 2) {
						const auto& a = p->GetArray();
						slice.PivotX = static_cast<float>(a[0].AsDoubleOr(0.5));
						slice.PivotY = static_cast<float>(a[1].AsDoubleOr(0.5));
					}
					meta.Sprites.push_back(std::move(slice));
				}
			}

			if (const Json::Value* cropNode = root.FindMember("crop"); cropNode && cropNode->IsObject()) {
				const Json::Value* enabledNode = cropNode->FindMember("enabled");
				const bool enabled = enabledNode && enabledNode->AsBoolOr(false);
				if (const Json::Value* r = cropNode->FindMember("rect"); r && r->IsArray() && r->GetArray().size() >= 4) {
					const auto& a = r->GetArray();
					meta.Crop.X = a[0].AsIntOr(0);
					meta.Crop.Y = a[1].AsIntOr(0);
					meta.Crop.W = a[2].AsIntOr(0);
					meta.Crop.H = a[3].AsIntOr(0);
				}
				meta.Crop.Enabled = enabled && meta.Crop.W > 0 && meta.Crop.H > 0;
			}

			return meta;
		}

		// Merge-write `import` and `sprites` blocks; preserves all other meta fields already on disk.
		static bool WriteTextureMeta(const std::string& assetPath, const TextureMeta& meta) {
			const std::string metaPath = GetMetaPath(assetPath);
			Json::Value root = LoadMetaJson(metaPath);
			if (!root.IsObject()) {
				root = Json::Value::MakeObject();
			}

			if (meta.HasImportBlock) {
				Json::Value importValue = Json::Value::MakeObject();
				importValue.AddMember("filter", Json::Value(std::string(magic_enum::enum_name(meta.Import.FilterMode))));
				importValue.AddMember("wrapU",  Json::Value(std::string(magic_enum::enum_name(meta.Import.WrapU))));
				importValue.AddMember("wrapV",  Json::Value(std::string(magic_enum::enum_name(meta.Import.WrapV))));
				SetOrAddMember(root, "import", std::move(importValue));
			}

			Json::Value spritesValue = Json::Value::MakeArray();
			for (const SpriteSlice& slice : meta.Sprites) {
				Json::Value spriteValue = Json::Value::MakeObject();
				spriteValue.AddMember("name", Json::Value(slice.Name));
				Json::Value rectValue = Json::Value::MakeArray();
				rectValue.Append(Json::Value(slice.X));
				rectValue.Append(Json::Value(slice.Y));
				rectValue.Append(Json::Value(slice.W));
				rectValue.Append(Json::Value(slice.H));
				spriteValue.AddMember("rect", std::move(rectValue));
				Json::Value pivotValue = Json::Value::MakeArray();
				pivotValue.Append(Json::Value(static_cast<double>(slice.PivotX)));
				pivotValue.Append(Json::Value(static_cast<double>(slice.PivotY)));
				spriteValue.AddMember("pivot", std::move(pivotValue));
				spritesValue.Append(std::move(spriteValue));
			}
			SetOrAddMember(root, "sprites", std::move(spritesValue));

			// Write `crop` only when enabled; if disabled, overwrite an
			// existing block (the Json API has no member-remove) so a crop
			// can be turned off, and leave never-cropped metas untouched.
			if (meta.Crop.Enabled) {
				Json::Value cropValue = Json::Value::MakeObject();
				cropValue.AddMember("enabled", Json::Value(true));
				Json::Value rectValue = Json::Value::MakeArray();
				rectValue.Append(Json::Value(meta.Crop.X));
				rectValue.Append(Json::Value(meta.Crop.Y));
				rectValue.Append(Json::Value(meta.Crop.W));
				rectValue.Append(Json::Value(meta.Crop.H));
				cropValue.AddMember("rect", std::move(rectValue));
				SetOrAddMember(root, "crop", std::move(cropValue));
			}
			else if (root.FindMember("crop")) {
				Json::Value cropValue = Json::Value::MakeObject();
				cropValue.AddMember("enabled", Json::Value(false));
				SetOrAddMember(root, "crop", std::move(cropValue));
			}

			const bool wrote = File::WriteAllText(metaPath, Json::Stringify(root, true));
			if (wrote) {
				s_MetaIdCache.erase(metaPath);
			}
			return wrote;
		}

	private:
		static std::string ToString(AssetKind kind) {
			switch (kind) {
			case AssetKind::Texture: return "Texture";
			case AssetKind::Audio: return "Audio";
			case AssetKind::Scene: return "Scene";
			case AssetKind::Prefab: return "Prefab";
			case AssetKind::Script: return "Script";
			case AssetKind::Font: return "Font";
			case AssetKind::Shader: return "Shader";
			case AssetKind::Other: return "Other";
			case AssetKind::Unknown:
			default:
				return "Unknown";
			}
		}

		static std::string ToLowerCopy(std::string value) {
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			return value;
		}

	private:
		inline static bool s_Dirty = true;
		inline static std::string s_TrackedRoot;
		inline static std::unordered_map<uint64_t, Record> s_IdToRecord;
		inline static std::unordered_map<std::string, uint64_t> s_PathToId;

		struct MetaIdCacheEntry {
			std::filesystem::file_time_type Mtime{};
			std::uintmax_t Size = 0;
			uint64_t AssetGuid = 0;
		};
		inline static std::unordered_map<std::string, MetaIdCacheEntry> s_MetaIdCache;
		// Cleared on MarkDirty()/Rebuild() so FileWatcher events let recovered assets re-resolve.
		inline static std::unordered_set<uint64_t> s_KnownMissingIds;
		inline static std::unordered_map<uint64_t, Record> s_BuiltInById;
		inline static std::unordered_map<std::string, uint64_t> s_BuiltInPathToId;
	};

}
