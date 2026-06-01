#pragma once

// NativeEntityCommandBuffer — batch entity/component ops; playback is ~20-50x faster than per-entity calls; default-constructs so C++ member initializers fire. NOT thread-safe for concurrent recording.

#include "Core/Export.hpp"
#include "Scene/EntityHandle.hpp"
#include "Scene/Scene.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

namespace Index {

	class Scene;

	class INDEX_API NativeEntityCommandBuffer {
	public:
		// Opaque handle to an entity recorded by this buffer. The wrapped
		// index is local to the buffer — it cannot be cross-used between
		// instances and is invalidated by Clear()/Playback().
		struct EntityRef {
			uint32_t Index;
		};

		NativeEntityCommandBuffer() = default;
		~NativeEntityCommandBuffer();

		NativeEntityCommandBuffer(const NativeEntityCommandBuffer&) = delete;
		NativeEntityCommandBuffer& operator=(const NativeEntityCommandBuffer&) = delete;
		NativeEntityCommandBuffer(NativeEntityCommandBuffer&&) noexcept = default;
		NativeEntityCommandBuffer& operator=(NativeEntityCommandBuffer&&) noexcept = default;

		// Records the creation of a fresh runtime-origin entity and returns
		// a handle that subsequent AddComponent calls reference. The handle
		// is stable until Clear() or destruction.
		EntityRef CreateEntity();

		// Children addressable only via root's children list AFTER Playback, not via additional ECB records.
		EntityRef Instantiate(uint64_t prefabGuid);

		EntityRef Instantiate(class Entity prefabAsset);

		template <typename T>
		std::enable_if_t<!std::is_empty_v<T>> AddComponent(EntityRef e);

		template <typename T>
		std::enable_if_t<!std::is_empty_v<T>> AddComponent(EntityRef e, T value);

		// Configure overload: default-constructs T then calls configure(component); engine defaults apply to untouched fields.
		// Capture size limit: kInlineStorageBytes (32 bytes); larger captures fail to compile with a clear message.
		template <typename T, typename Configure>
		std::enable_if_t<!std::is_empty_v<T>>
		AddComponent(EntityRef e, Configure&& configure);

		// Tag (empty-type) variant. No payload, no patch.
		template <typename T>
		std::enable_if_t<std::is_empty_v<T>> AddComponent(EntityRef e);

		template <typename... Ts>
		EntityRef CreateWith();

		// output.size() >= length must hold; fills output[0..length) with created EntityRefs in order.
		template <typename... Ts>
		void CreateEntitiesWith(int length, std::span<EntityRef> output);

		int Playback(Scene& scene);

		// Discards commands without releasing the backing buffer; trivially-destructible captures skip destructor calls.
		void Clear();

		// Number of entities queued so far (before Playback). After
		// Playback, this is the number of entities that were created.
		int EntityCount() const { return static_cast<int>(m_EntityCount); }

		// Number of recorded AddComponent commands so far.
		int CommandCount() const { return static_cast<int>(m_Commands.size()); }

		EntityHandle GetCreatedEntity(int i) const;

		static constexpr std::size_t kInlineStorageBytes = 32;

	private:
		// One per recorded AddComponent. Fits in 56 bytes on x64 — one cache line.
		struct Command {
			uint32_t EntityIndex;
			// Takes Scene& (not registry&) so prefab-instantiate thunks can call SceneSerializer.
			// `handle` is a reference so the Instantiate thunk can rewrite m_CreatedHandles[EntityIndex] with the prefab root.
			void (*Apply)(void* state, Scene& scene, EntityHandle& handle);
			void (*Destroy)(void* state); // nullptr for trivially-destructible captures
			alignas(std::max_align_t) std::byte Storage[kInlineStorageBytes];
		};
		static_assert(sizeof(Command) <= 64, "Command must fit in one cache line");

		// Reuses storage across Clear() cycles — per-frame spawn loops stay
		// allocation-free once the buffer reaches steady-state size.
		std::vector<Command>      m_Commands;
		std::vector<EntityHandle> m_CreatedHandles;
		uint32_t                  m_EntityCount = 0;

		Command& AppendCommand(uint32_t entityIndex,
			void (*apply)(void*, Scene&, EntityHandle&),
			void (*destroy)(void*));

		// Lets ParallelEntityCommandBuffer reach into the merge details
		// (entity count and command list) without exposing them publicly.
		friend class ParallelEntityCommandBuffer;
	};

	// ───────────────────────────── inline template definitions ───────────

	template <typename T>
	std::enable_if_t<!std::is_empty_v<T>>
	NativeEntityCommandBuffer::AddComponent(EntityRef e) {
		AppendCommand(e.Index,
			[](void*, Scene& scene, EntityHandle& handle) {
				if constexpr (std::is_default_constructible_v<T>) {
					scene.GetRegistry().emplace<T>(handle);
				}
				else {
					static_assert(std::is_default_constructible_v<T>,
						"AddComponent<T>(e) requires T to be default-constructible. "
						"Use the value or patch overload for components without a "
						"default constructor.");
				}
			},
			nullptr);
	}

	template <typename T>
	std::enable_if_t<!std::is_empty_v<T>>
	NativeEntityCommandBuffer::AddComponent(EntityRef e, T value) {
		static_assert(sizeof(T) <= kInlineStorageBytes,
			"Component value is larger than the ECB inline storage (32 bytes). "
			"Use the patch overload (lambda) and refer to heap state by "
			"reference, or split this component.");
		static_assert(alignof(T) <= alignof(std::max_align_t),
			"Component alignment exceeds NativeEntityCommandBuffer storage alignment.");

		Command& cmd = AppendCommand(e.Index,
			[](void* state, Scene& scene, EntityHandle& handle) {
				T* held = std::launder(reinterpret_cast<T*>(state));
				scene.GetRegistry().emplace<T>(handle, std::move(*held));
			},
			std::is_trivially_destructible_v<T>
				? nullptr
				: +[](void* state) {
					std::launder(reinterpret_cast<T*>(state))->~T();
				});
		::new (cmd.Storage) T(std::move(value));
	}

	template <typename T, typename Configure>
	std::enable_if_t<!std::is_empty_v<T>>
	NativeEntityCommandBuffer::AddComponent(EntityRef e, Configure&& configure) {
		using Patch = std::decay_t<Configure>;
		static_assert(sizeof(Patch) <= kInlineStorageBytes,
			"Captured patch lambda is larger than the ECB inline storage "
			"(32 bytes). Capture less by-value, or close over a pointer to "
			"the heap state instead.");
		static_assert(alignof(Patch) <= alignof(std::max_align_t),
			"Patch alignment exceeds NativeEntityCommandBuffer storage alignment.");
		static_assert(std::is_default_constructible_v<T>,
			"AddComponent<T>(e, configure) default-constructs T before applying "
			"the patch. T must be default-constructible — use the value overload "
			"for components without a default constructor.");
		static_assert(std::is_invocable_v<Patch&, T&>,
			"Patch callable must be invocable as (T&). Example shape: "
			"[](Transform2DComponent& tr) { tr.Position = ...; }");

		Command& cmd = AppendCommand(e.Index,
			[](void* state, Scene& scene, EntityHandle& handle) {
				Patch* patch = std::launder(reinterpret_cast<Patch*>(state));
				T& component = scene.GetRegistry().emplace<T>(handle);
				(*patch)(component);
			},
			std::is_trivially_destructible_v<Patch>
				? nullptr
				: +[](void* state) {
					std::launder(reinterpret_cast<Patch*>(state))->~Patch();
				});
		::new (cmd.Storage) Patch(std::forward<Configure>(configure));
	}

	template <typename T>
	std::enable_if_t<std::is_empty_v<T>>
	NativeEntityCommandBuffer::AddComponent(EntityRef e) {
		AppendCommand(e.Index,
			[](void*, Scene& scene, EntityHandle& handle) {
				scene.GetRegistry().emplace<T>(handle);
			},
			nullptr);
	}

	template <typename... Ts>
	NativeEntityCommandBuffer::EntityRef
	NativeEntityCommandBuffer::CreateWith() {
		EntityRef e = CreateEntity();
		(AddComponent<Ts>(e), ...);
		return e;
	}

	template <typename... Ts>
	void NativeEntityCommandBuffer::CreateEntitiesWith(int length, std::span<EntityRef> output) {
		if (length < 0 || static_cast<std::size_t>(length) > output.size()) {
			return;
		}
		for (int i = 0; i < length; ++i) {
			output[i] = CreateWith<Ts...>();
		}
	}

} // namespace Index
