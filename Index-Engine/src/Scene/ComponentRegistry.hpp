#pragma once
#include "Core/Assert.hpp"
#include "Core/Exceptions.hpp"
#include "Core/Log.hpp"
#include "Components/Tags.hpp"
#include "Scene/ComponentInfo.hpp"
#include "Scene/DynamicComponentStorage.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Index::detail {
    // FNV-1a 64-bit, stable across ABIs — safe to embed in serialized assets.
    constexpr uint64_t FnvHash64(std::string_view str) noexcept {
        constexpr uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
        constexpr uint64_t kPrime = 0x100000001b3ULL;
        uint64_t h = kOffsetBasis;
        for (char c : str) {
            h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
            h *= kPrime;
        }
        return h;
    }
}

namespace Index {
    class ComponentRegistry {
    public:
        template<typename T>
        void Register(ComponentInfo info) {
            const std::type_index id = typeid(T);
            const auto existing = m_map.find(id);
            if (existing != m_map.end()) {
                if (info.serializedName.empty()) info.serializedName = existing->second.serializedName;
                if (!info.drawInspector) info.drawInspector = existing->second.drawInspector;
                if (info.properties.empty()) info.properties = existing->second.properties;
                if (info.conflictsWith.empty()) info.conflictsWith = existing->second.conflictsWith;
                if (info.dependsOn.empty()) info.dependsOn = existing->second.dependsOn;
                // Preserve serialize/deserialize: AttachInspector re-registers and would otherwise drop them.
                if (!info.serialize) info.serialize = existing->second.serialize;
                if (!info.deserialize) info.deserialize = existing->second.deserialize;
                if (!info.serializeArchive) info.serializeArchive = existing->second.serializeArchive;
                if (info.serializationVersion == 1 && existing->second.serializationVersion != 1) {
                    info.serializationVersion = existing->second.serializationVersion;
                }
                if (!info.onAdd) info.onAdd = existing->second.onAdd;
                // typeIdU32 is published to the managed side and embedded in ECB wire payloads — must survive re-registration.
                if (info.typeIdU32 == 0) info.typeIdU32 = existing->second.typeIdU32;
                if (!info.emplaceFromBytes) info.emplaceFromBytes = existing->second.emplaceFromBytes;
                if (!info.writeBytes) info.writeBytes = existing->second.writeBytes;
                if (!info.defaultEmplace) info.defaultEmplace = existing->second.defaultEmplace;
            }

            info.typeId = id;
            info.storageHash = entt::type_hash<T>::value();
            info.has = [](Entity e) { return e.HasComponent<T>(); };
            info.add = [](Entity e) { e.AddComponent<T>(); };
            info.remove = [](Entity e) { e.RemoveComponent<T>(); };

            // Skip empty tag types — no addressable payload in EnTT's empty-type storage.
            if constexpr (!std::is_empty_v<T>) {
                info.getRaw = [](Entity e) -> void* {
                    if (!e.HasComponent<T>()) return nullptr;
                    return static_cast<void*>(&e.GetComponent<T>());
                };
                info.fillRawPointers = [](entt::registry& registry, void** outPointers, int maxRows, int enableFilter) -> int {
                    int count = 0;
                    auto view = registry.view<T>();
                    for (auto&& [entity, component] : view.each()) {
                        if (enableFilter == 1 && registry.all_of<DisabledTag>(entity)) continue;
                        if (enableFilter == 2 && !registry.all_of<DisabledTag>(entity)) continue;
                        if (outPointers && count < maxRows) {
                            outPointers[count] = static_cast<void*>(&component);
                        }
                        ++count;
                    }
                    return count;
                };
                info.rawSize = sizeof(T);

                // Gate on is_trivially_destructible (not is_trivially_copyable): UUID has a user-declared
                // copy-ctor but a trivial dtor, making SpriteRendererComponent memcpy-safe despite being
                // non-trivially-copyable. Components with owning heap (std::vector etc.) are excluded —
                // null emplaceFromBytes surfaces a clean error instead of a silent double-free.
                if constexpr (std::is_trivially_destructible_v<T>) {
                    if (info.emplaceFromBytes == nullptr) {
                        info.emplaceFromBytes = [](entt::registry& r, EntityHandle e,
                                                   const void* bytes, size_t size) {
                            IDX_CORE_ASSERT(size == sizeof(T), IndexErrorCode::InvalidValue,
                                "ComponentRegistry: emplaceFromBytes size mismatch for component");
                            T value;
                            std::memcpy(&value, bytes, sizeof(T));
                            r.emplace_or_replace<T>(e, std::move(value));
                        };
                    }
                }

                if (info.defaultEmplace == nullptr) {
                    if constexpr (std::is_default_constructible_v<T>) {
                        info.defaultEmplace = [](entt::registry& r, EntityHandle e) {
                            r.emplace<T>(e);
                        };
                    }
                }

                // Same trivially-destructible gate as emplaceFromBytes; null writeBytes makes PrefabTemplateCache
                // mark the template unbakeable and fall back to the slow per-property path.
                if constexpr (std::is_trivially_destructible_v<T>) {
                    if (info.writeBytes == nullptr) {
                        info.writeBytes = [](const entt::registry& r, EntityHandle e,
                                              std::vector<uint8_t>& out) -> bool {
                            const T* comp = r.try_get<T>(e);
                            if (comp == nullptr) return false;
                            const size_t oldSize = out.size();
                            out.resize(oldSize + sizeof(T));
                            std::memcpy(out.data() + oldSize, comp, sizeof(T));
                            return true;
                        };
                    }
                }
            }

            // Preserve custom copyTo — scene-bound components (e.g. ParticleSystem2D) need re-binding
            // that the raw value-copy path bypasses.
            const bool preserveExistingCopyTo =
                info.copyTo != nullptr ||
                (existing != m_map.end() && existing->second.copyTo != nullptr);
            if (preserveExistingCopyTo && info.copyTo == nullptr) {
                info.copyTo = existing->second.copyTo;
            }

            if (!preserveExistingCopyTo) {
                if constexpr (!std::is_empty_v<T>) {
                    info.copyTo = [](Entity src, Entity dst) {
                        if (!src.HasComponent<T>()) return;
                        if (dst.HasComponent<T>())
                            dst.GetComponent<T>() = src.GetComponent<T>();
                        else
                            dst.AddComponent<T>(src.GetComponent<T>());
                    };
                } else {
                    info.copyTo = [](Entity src, Entity dst) {
                        if (src.HasComponent<T>() && !dst.HasComponent<T>())
                            dst.AddComponent<T>();
                    };
                }
            }

            info.serializedNameHash = info.serializedName.empty()
                ? 0u
                : detail::FnvHash64(info.serializedName);

            if (existing != m_map.end()) {
                const uint64_t oldHash = existing->second.serializedNameHash;
                if (oldHash != 0 && oldHash != info.serializedNameHash) {
                    const auto oldIt = m_hashIndex.find(oldHash);
                    if (oldIt != m_hashIndex.end() && oldIt->second == &existing->second) {
                        m_hashIndex.erase(oldIt);
                    }
                }
            }

            if (info.typeIdU32 == 0) {
                if (m_byTypeId.empty()) {
                    m_byTypeId.push_back(nullptr); // reserve slot 0 as null
                }
                info.typeIdU32 = static_cast<uint32_t>(m_byTypeId.size());
                m_byTypeId.push_back(nullptr); // placeholder, fixed up below
            }

            auto inserted = m_map.insert_or_assign(id, std::move(info));
            ComponentInfo& stored = inserted.first->second;
            if (stored.serializedNameHash != 0) {
                m_hashIndex[stored.serializedNameHash] = &stored;
            }
            if (stored.typeIdU32 != 0 && stored.typeIdU32 < m_byTypeId.size()) {
                m_byTypeId[stored.typeIdU32] = &stored;
            }
        }

        /// O(1) lookup by stable u32 type ID. Returns nullptr for id 0 (sentinel) or out-of-range ids.
        const ComponentInfo* GetByTypeId(uint32_t typeIdU32) const {
            if (typeIdU32 == 0 || typeIdU32 >= m_byTypeId.size()) return nullptr;
            return m_byTypeId[typeIdU32];
        }

        uint32_t GetTypeIdCount() const {
            // Slot 0 is the null sentinel — subtract it from the reported count.
            return m_byTypeId.empty() ? 0u : static_cast<uint32_t>(m_byTypeId.size() - 1);
        }

        const auto& All() const { return m_map; }

        /// O(1) lookup by FNV-1a hash of serializedName. Returns nullptr for hash 0 or no match.
        const ComponentInfo* FindByHash(uint64_t serializedNameHash) const {
            if (serializedNameHash == 0) return nullptr;
            const auto it = m_hashIndex.find(serializedNameHash);
            return it != m_hashIndex.end() ? it->second : nullptr;
        }

        const ComponentInfo* FindBySerializedName(std::string_view name) const {
            return FindByHash(detail::FnvHash64(name));
        }

        /// Resolve ComponentInfo for hybrid inspectors that mix DrawAll with custom widgets.
        template <typename T>
        const ComponentInfo* GetInfo() const {
            const auto it = m_map.find(typeid(T));
            return it != m_map.end() ? &it->second : nullptr;
        }

        template <typename F>
        void ForEachComponentInfo(F&& fn) {
            for (auto& [id, info] : m_map)
                fn(id, info);
            // Dynamic entries use typeid(void) — callers keying by type_index implicitly skip them.
            for (auto& [tid, info] : m_dynamicMap) {
                (void)tid;
                fn(s_VoidTypeIndex, info);
            }
        }

        template <typename F>
        void ForEachComponentInfo(F&& fn) const {
            for (const auto& [id, info] : m_map)
                fn(id, info);
            for (const auto& [tid, info] : m_dynamicMap) {
                (void)tid;
                fn(s_VoidTypeIndex, info);
            }
        }

        // RegisterDynamic: called by DynamicComponentRegistrar after user assembly loads. Returns assigned
        // typeIdU32 or 0 on failure. Storage is owned here, outliving all captured callbacks until UnregisterAllDynamic.
        uint32_t RegisterDynamic(
            const std::string& displayName,
            const std::string& serializedName,
            const std::string& subcategory,
            ComponentCategory category,
            uint32_t size,
            uint32_t alignment)
        {
            if (size == 0) {
                IDX_CORE_WARN_TAG("ComponentRegistry",
                    "RegisterDynamic refused '{}': size is zero", displayName);
                return 0;
            }
            if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
                IDX_CORE_WARN_TAG("ComponentRegistry",
                    "RegisterDynamic refused '{}': alignment {} is not a power of two",
                    displayName, alignment);
                return 0;
            }
            if (alignment > alignof(std::max_align_t)) {
                IDX_CORE_WARN_TAG("ComponentRegistry",
                    "RegisterDynamic refused '{}': alignment {} exceeds max_align_t ({})",
                    displayName, alignment, static_cast<uint32_t>(alignof(std::max_align_t)));
                return 0;
            }
            if ((size % alignment) != 0) {
                IDX_CORE_WARN_TAG("ComponentRegistry",
                    "RegisterDynamic refused '{}': size {} is not a multiple of alignment {} "
                    "(successive elements would land at misaligned offsets)",
                    displayName, size, alignment);
                return 0;
            }
            const uint64_t serializedHash = serializedName.empty()
                ? 0u : detail::FnvHash64(serializedName);
            if (serializedHash != 0 && m_hashIndex.find(serializedHash) != m_hashIndex.end()) {
                IDX_CORE_WARN_TAG("ComponentRegistry",
                    "RegisterDynamic refused '{}': serializedName '{}' already taken",
                    displayName, serializedName);
                return 0;
            }

            auto storage = std::make_unique<DynamicComponentStorage>(size, alignment);
            DynamicComponentStorage* storagePtr = storage.get();

            ComponentInfo info{};
            info.displayName       = displayName;
            info.serializedName    = serializedName;
            info.subcategory       = subcategory;
            info.category          = category;
            info.serializedNameHash = serializedHash;
            info.isDynamic         = true;
            info.rawSize           = size;
            info.dynamicStorage    = storagePtr;
            info.has = [storagePtr](Entity e) -> bool {
                return e.IsValid() && storagePtr->Contains(e.GetHandle());
            };
            info.add = [storagePtr](Entity e) {
                if (e.IsValid()) storagePtr->Add(e.GetHandle());
            };
            info.remove = [storagePtr](Entity e) {
                if (e.IsValid()) storagePtr->Remove(e.GetHandle());
            };
            info.getRaw = [storagePtr](Entity e) -> void* {
                if (!e.IsValid()) return nullptr;
                return storagePtr->Get(e.GetHandle());
            };
            info.emplaceFromBytes = [storagePtr](entt::registry&, EntityHandle e,
                const void* bytes, size_t byteSize) {
                IDX_CORE_ASSERT(byteSize == storagePtr->ElementSize(),
                    IndexErrorCode::InvalidValue,
                    "Dynamic component emplaceFromBytes size mismatch");
                storagePtr->EmplaceOrReplace(e, bytes);
            };
            info.defaultEmplace = [storagePtr](entt::registry&, EntityHandle e) {
                storagePtr->Add(e);
            };
            info.copyTo = [storagePtr](Entity src, Entity dst) {
                if (!src.IsValid() || !dst.IsValid()) return;
                const void* srcBytes = storagePtr->Get(src.GetHandle());
                if (!srcBytes) return;
                storagePtr->EmplaceOrReplace(dst.GetHandle(), srcBytes);
            };
            info.fillRawPointers = [storagePtr](entt::registry& reg, void** outPointers,
                int maxRows, int enableFilter) -> int {
                // Snapshot before iterating: filter checks can fire on_construct/on_destroy hooks that mutate storage.
                std::vector<EntityHandle> entitiesSnapshot = storagePtr->Entities();
                int count = 0;
                for (EntityHandle e : entitiesSnapshot) {
                    if (enableFilter == 1 && reg.all_of<DisabledTag>(e)) continue;
                    if (enableFilter == 2 && !reg.all_of<DisabledTag>(e)) continue;
                    if (outPointers && count < maxRows) {
                        outPointers[count] = storagePtr->Get(e);
                    }
                    ++count;
                }
                return count;
            };

            info.serialize = [storagePtr](Entity e) -> Json::Value {
                if (!e.IsValid() || !storagePtr) return Json::Value::MakeObject();
                const void* bytes = storagePtr->Get(e.GetHandle());
                if (!bytes || storagePtr->ElementSize() == 0) {
                    return Json::Value::MakeObject();
                }
                static constexpr char kHex[] = "0123456789abcdef";
                std::string hex;
                const uint32_t n = storagePtr->ElementSize();
                hex.resize(static_cast<std::size_t>(n) * 2);
                const std::uint8_t* src = static_cast<const std::uint8_t*>(bytes);
                for (uint32_t i = 0; i < n; ++i) {
                    hex[i * 2 + 0] = kHex[(src[i] >> 4) & 0x0F];
                    hex[i * 2 + 1] = kHex[(src[i] >> 0) & 0x0F];
                }
                Json::Value out = Json::Value::MakeObject();
                out.AddMember("data", Json::Value(std::move(hex)));
                return out;
            };

            info.deserialize = [storagePtr](Entity e, const Json::Value& v) {
                if (!e.IsValid() || !storagePtr) return;
                if (!v.IsObject()) return;
                const Json::Value* dataNode = v.FindMember("data");
                if (!dataNode || !dataNode->IsString()) return;
                const std::string hex = dataNode->AsStringOr();
                const std::size_t byteCount = hex.size() / 2;
                if (byteCount == 0 || byteCount != storagePtr->ElementSize()) return;
                std::vector<std::uint8_t> bytes(byteCount);
                auto fromHex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                    return -1;
                };
                for (std::size_t i = 0; i < byteCount; ++i) {
                    const int hi = fromHex(hex[i * 2 + 0]);
                    const int lo = fromHex(hex[i * 2 + 1]);
                    if (hi < 0 || lo < 0) return; // malformed hex — leave zero-init
                    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
                }
                storagePtr->EmplaceOrReplace(e.GetHandle(), bytes.data());
            };

            if (m_byTypeId.empty()) m_byTypeId.push_back(nullptr);
            info.typeIdU32 = static_cast<uint32_t>(m_byTypeId.size());
            m_byTypeId.push_back(nullptr); // placeholder; patched below

            info.storageHash = static_cast<entt::id_type>(serializedHash != 0
                ? serializedHash : static_cast<uint64_t>(info.typeIdU32));

            const uint32_t typeIdU32 = info.typeIdU32;
            m_dynamicStorages.emplace(typeIdU32, std::move(storage));
            auto inserted = m_dynamicMap.emplace(typeIdU32, std::move(info));
            ComponentInfo& stored = inserted.first->second;
            if (stored.serializedNameHash != 0) {
                m_hashIndex[stored.serializedNameHash] = &stored;
            }
            if (stored.typeIdU32 < m_byTypeId.size()) {
                m_byTypeId[stored.typeIdU32] = &stored;
            }
            return typeIdU32;
        }

        // Dynamics live outside EnTT pools so on_destroy won't fire; call before m_Registry.destroy().
        void ScrubEntity(EntityHandle e) {
            for (auto& [typeIdU32, storage] : m_dynamicStorages) {
                (void)typeIdU32;
                if (storage) {
                    storage->Remove(e);
                }
            }
        }

        // MUST be called before user assembly unloads so captured lambdas don't outlive storage.
        void UnregisterAllDynamic() {
            for (auto& [typeIdU32, info] : m_dynamicMap) {
                if (info.serializedNameHash != 0) {
                    const auto it = m_hashIndex.find(info.serializedNameHash);
                    if (it != m_hashIndex.end() && it->second == &info) {
                        m_hashIndex.erase(it);
                    }
                }
                if (typeIdU32 < m_byTypeId.size()) {
                    m_byTypeId[typeIdU32] = nullptr;
                }
            }
            m_dynamicMap.clear();
            m_dynamicStorages.clear();
        }

        void CopyComponents(Entity src, Entity dst) const {
            for (const auto& [id, info] : m_map) {
                (void)id;
                if (info.copyTo) {
                    info.copyTo(src, dst);
                }
            }
        }

        bool HasConflict(Entity entity, std::type_index proposed) const {
            const auto proposedIt = m_map.find(proposed);
            const ComponentInfo* proposedInfo = (proposedIt != m_map.end()) ? &proposedIt->second : nullptr;

            for (const auto& [existingId, existingInfo] : m_map) {
                if (existingId == proposed) continue;
                if (!existingInfo.has || !existingInfo.has(entity)) continue;

                // proposed → existing
                if (proposedInfo) {
                    for (const std::type_index& conflict : proposedInfo->conflictsWith) {
                        if (conflict == existingId) return true;
                    }
                }
                // existing → proposed
                for (const std::type_index& conflict : existingInfo.conflictsWith) {
                    if (conflict == proposed) return true;
                }
            }
            return false;
        }

        /// Adds component + transitive dependsOn chain. Cycle-safe. Conflicting deps are skipped with a warning.
        bool AddWithDependencies(Entity entity, std::type_index type) const {
            std::unordered_set<std::type_index> visited;
            return AddWithDependenciesImpl(entity, type, visited, /*isRoot=*/true);
        }

        /// Validates conflict symmetry (A↔B both declared). O(N*M), debug-only.
#ifdef IDX_DEBUG
        void ValidateConflictSymmetry() const {
            for (const auto& [aId, aInfo] : m_map) {
                for (const std::type_index& bId : aInfo.conflictsWith) {
                    const auto bIt = m_map.find(bId);
                    bool symmetric = false;
                    if (bIt != m_map.end()) {
                        for (const std::type_index& back : bIt->second.conflictsWith) {
                            if (back == aId) { symmetric = true; break; }
                        }
                    }
                    IDX_CORE_ASSERT(symmetric, IndexErrorCode::InvalidValue,
                        std::string("Asymmetric conflict declaration: ") + aId.name() + " <-> " + bId.name() +
                        " (declared on first side only - DeclareConflict<A, B>() should add both directions)");
                }
            }
        }
#else
        void ValidateConflictSymmetry() const {}
#endif

        bool TypesConflict(std::type_index a, std::type_index b) const {
            if (a == b) return false;
            const auto ai = m_map.find(a);
            if (ai != m_map.end()) {
                for (const std::type_index& c : ai->second.conflictsWith) {
                    if (c == b) return true;
                }
            }
            const auto bi = m_map.find(b);
            if (bi != m_map.end()) {
                for (const std::type_index& c : bi->second.conflictsWith) {
                    if (c == a) return true;
                }
            }
            return false;
        }

    private:
        bool AddWithDependenciesImpl(Entity entity, std::type_index type,
            std::unordered_set<std::type_index>& visited, bool isRoot) const
        {
            if (!visited.insert(type).second) return true;

            const auto it = m_map.find(type);
            if (it == m_map.end() || !it->second.add || !it->second.has) return false;
            const ComponentInfo& info = it->second;

            if (info.has(entity)) return true;

            if (HasConflict(entity, type)) {
                if (!isRoot) {
                    IDX_CORE_WARN_TAG("ComponentRegistry",
                        "Skipping auto-add of dependency '{}' — conflicts with a component already on the entity",
                        info.displayName.empty() ? type.name() : info.displayName);
                }
                return false;
            }

            for (const std::type_index& dep : info.dependsOn) {
                AddWithDependenciesImpl(entity, dep, visited, /*isRoot=*/false);
            }

            info.add(entity);
            if (info.onAdd) {
                info.onAdd(entity);
            }
            return true;
        }

        std::unordered_map<std::type_index, ComponentInfo> m_map;
        std::unordered_map<uint64_t, const ComponentInfo*> m_hashIndex;
        std::vector<const ComponentInfo*> m_byTypeId; // 1-indexed; slot 0 = null sentinel
        std::unordered_map<uint32_t, ComponentInfo> m_dynamicMap;
        std::unordered_map<uint32_t, std::unique_ptr<DynamicComponentStorage>> m_dynamicStorages;
        // Dynamics share typeid(void) — std::type_index can't be synthesized; callers that key by
        // type_index implicitly skip dynamic entries.
        static inline const std::type_index s_VoidTypeIndex{ typeid(void) };
    };
}
