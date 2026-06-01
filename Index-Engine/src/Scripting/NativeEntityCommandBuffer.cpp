#include "pch.hpp"

#include "Scripting/NativeEntityCommandBuffer.hpp"

#include "Components/General/EntityMetaDataComponent.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Serialization/SceneSerializer.hpp"

namespace Index {

	NativeEntityCommandBuffer::~NativeEntityCommandBuffer() {
		// Clear() handles destructor invocation for any still-pending commands.
		Clear();
	}

	NativeEntityCommandBuffer::EntityRef NativeEntityCommandBuffer::CreateEntity() {
		EntityRef ref{ m_EntityCount };
		++m_EntityCount;
		return ref;
	}

	NativeEntityCommandBuffer::EntityRef
	NativeEntityCommandBuffer::Instantiate(uint64_t prefabGuid) {
		EntityRef ref{ m_EntityCount };
		++m_EntityCount;
		// Thunk replaces the bulk-create placeholder with the real prefab tree, rewriting the handle slot so subsequent AddComponent commands land on the root.
		Command& cmd = AppendCommand(ref.Index,
			[](void* state, Scene& scene, EntityHandle& handleSlot) {
				const uint64_t guid = *std::launder(reinterpret_cast<uint64_t*>(state));
				if (scene.GetRegistry().valid(handleSlot)) {
					scene.DestroyEntity(handleSlot);
				}
				EntityHandle newRoot = SceneSerializer::InstantiatePrefab(scene, guid);
				handleSlot = newRoot;
			},
			nullptr);
		static_assert(sizeof(uint64_t) <= kInlineStorageBytes,
			"prefab GUID payload must fit in the inline command storage");
		::new (cmd.Storage) uint64_t(prefabGuid);
		return ref;
	}

	NativeEntityCommandBuffer::EntityRef
	NativeEntityCommandBuffer::Instantiate(class Entity prefabAsset) {
		if (!prefabAsset.IsValid()) {
			// Empty ref signals "no recorded entity". Caller can
			// detect via EntityCount() not incrementing.
			return EntityRef{ 0 };
		}
		const AssetGUID guid = prefabAsset.GetPrefabGUID();
		if (static_cast<uint64_t>(guid) == 0) {
			return EntityRef{ 0 };
		}
		return Instantiate(static_cast<uint64_t>(guid));
	}

	NativeEntityCommandBuffer::Command& NativeEntityCommandBuffer::AppendCommand(
		uint32_t entityIndex,
		void (*apply)(void*, Scene&, EntityHandle&),
		void (*destroy)(void*))
	{
		Command& cmd = m_Commands.emplace_back();
		cmd.EntityIndex = entityIndex;
		cmd.Apply = apply;
		cmd.Destroy = destroy;
		// Storage stays uninitialized — the caller placement-news into it.
		return cmd;
	}

	int NativeEntityCommandBuffer::Playback(Scene& scene) {
		// Empty buffer is a valid no-op so callers wiring an ECB into a
		// generic spawn pipeline don't have to special-case "no work."
		if (m_EntityCount == 0) {
			return 0;
		}

		scene.ReserveForLoadRuntime(m_EntityCount, {});

		m_CreatedHandles.resize(m_EntityCount);
		scene.CreateEntitiesBulk(m_EntityCount, m_CreatedHandles);

		// LoadGuard defers on_construct hooks to end-of-scope instead of firing per-emplace.
		Scene::LoadGuard guard(scene);

		// Stamp runtime metadata on every entity — same as the managed ECB
		// (ScriptBindingsEcb.cpp:153). NoFlags variant skips the per-entity
		// dirty pulse; we do a single MarkAllDirtyOnce() at the end.
		for (uint32_t i = 0; i < m_EntityCount; ++i) {
			scene.SetEntityMetaDataNoFlags(m_CreatedHandles[i], EntityOrigin::Runtime);
		}

		for (Command& cmd : m_Commands) {
			if (cmd.EntityIndex < m_EntityCount) {
				cmd.Apply(cmd.Storage, scene, m_CreatedHandles[cmd.EntityIndex]);
			}
		}

		scene.MarkAllDirtyOnce();

		return static_cast<int>(m_EntityCount);
	}

	void NativeEntityCommandBuffer::Clear() {
		for (Command& cmd : m_Commands) {
			if (cmd.Destroy != nullptr) {
				cmd.Destroy(cmd.Storage);
			}
		}
		m_Commands.clear();      // capacity retained
		m_EntityCount = 0;
		// m_CreatedHandles is kept sized to its peak so the next Playback
		// at the same scale doesn't realloc.
	}

	EntityHandle NativeEntityCommandBuffer::GetCreatedEntity(int i) const {
		if (i < 0 || static_cast<std::size_t>(i) >= m_CreatedHandles.size()) {
			return entt::null;
		}
		return m_CreatedHandles[static_cast<std::size_t>(i)];
	}

} // namespace Index
