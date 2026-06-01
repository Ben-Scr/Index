#pragma once

#include "Core/Export.hpp"
#include "Scene/EntityHandle.hpp"

#if INDEX_WITH_EDITOR
// std::unique_ptr<FileWatcher> member requires a complete type for the implicit destructor.
#include "Serialization/FileWatcher.hpp"
#endif

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Index {

	class Scene;

	// Baked prefab: first spawn serializes the live tree into payloadBlob; subsequent spawns replay from it, skipping disk/JSON/setter overhead.
	// Names are separate from the blob (NameComponent holds a std::string whose bytes would alias source-entity heap after memcpy).
	struct PrefabTemplate {
		struct EntitySlot {
			uint32_t parentIndex = UINT32_MAX;   // UINT32_MAX = root
			std::string name;                     // captured separately from byte blob
			uint64_t sourceUuid = 0;              // diagnostics only
			uint32_t componentRecordBegin = 0;    // index into `components`
			uint32_t componentRecordEnd = 0;      // exclusive
		};
		std::vector<EntitySlot> entities;

		struct ComponentRecord {
			uint32_t typeIdU32 = 0;
			uint32_t byteOffset = 0;              // into payloadBlob
			uint32_t byteSize = 0;
		};
		std::vector<ComponentRecord> components;
		std::vector<uint8_t> payloadBlob;

		// False when prefab contains internal entity refs or a component with unwired writeBytes; callers must use the slow path.
		bool bakeable = true;
		std::string unbakeableReason;

		uint32_t EntityCount() const { return static_cast<uint32_t>(entities.size()); }
	};

	class INDEX_API PrefabTemplateCache {
	public:
		static PrefabTemplateCache& Get();

		const PrefabTemplate* Find(uint64_t prefabGuid) const;

		// Non-zero fixupsAddedDuringDeserialize marks the template unbakeable (internal entity refs).
		void CaptureFromLive(uint64_t prefabGuid, Scene& scene,
			EntityHandle root,
			std::size_t fixupsAddedDuringDeserialize);

		EntityHandle Hydrate(uint64_t prefabGuid, Scene& scene);

		// preAllocated[0] is the root, rest are DFS children; caller owns LoadGuard/dirty-pulse (ECB playback already brackets these).
		EntityHandle HydrateInto(uint64_t prefabGuid, Scene& scene,
			std::span<const EntityHandle> preAllocated);

		void Invalidate(uint64_t prefabGuid);
		void InvalidateAll();

		std::size_t Size() const;

#if INDEX_WITH_EDITOR
		void InitializeForProject(const std::string& assetsRoot);
		void Shutdown();
#endif

	private:
		PrefabTemplateCache() = default;
		~PrefabTemplateCache();

		PrefabTemplateCache(const PrefabTemplateCache&) = delete;
		PrefabTemplateCache& operator=(const PrefabTemplateCache&) = delete;

		mutable std::shared_mutex m_Mutex;
		std::unordered_map<uint64_t, std::unique_ptr<PrefabTemplate>> m_Templates;

#if INDEX_WITH_EDITOR
		std::unique_ptr<FileWatcher> m_Watcher;
#endif
	};

} // namespace Index
