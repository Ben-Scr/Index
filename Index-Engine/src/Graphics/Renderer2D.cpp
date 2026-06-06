#include "pch.hpp"
#include "Graphics/Renderer2D.hpp"

#include "Components/General/Transform2DComponent.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/Graphics/PostProcessing2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Tags.hpp"
#include "Collections/Viewport.hpp"
#include "Core/Log.hpp"
#include "Core/Window.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/DynamicRenderData.hpp"
#include "Graphics/RenderApi.hpp"
#include "Graphics/StaticRenderData.hpp"
#include "Graphics/SpriteResources.hpp"
#include "Graphics/SpriteUVResolver.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Graphics/Text/TextRenderer.hpp"
#include "Math/Trigonometry.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/Scene.hpp"
#include "Profiling/Profiler.hpp"
#include "Jobs/JobSystem.hpp"
#include "Jobs/ParallelFor.hpp"
#ifdef INDEX_PROFILER_ENABLED
#include "Profiling/GpuTimer.hpp"
#endif

#include <webgpu/webgpu_cpp.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Renderer2D: persistent instance VBO (geometric growth), persistent UBO (64B viewProj), bind groups cached by texture pool ID
// and evicted via TextureManager::DestroyListener BEFORE GPU teardown; render pass uses LoadOp::Load to preserve prior Clear results.

namespace Index {

	namespace {
		using WebGPUSpriteResources::SpriteInstance;

		// Per-frame CPU scratch (persistent so the heap doesn't churn).
		std::vector<Instance44>     g_InstancesScratch;
		std::vector<TextureHandle>  g_TexturesScratch;
		std::vector<SpriteInstance> g_GpuInstanceScratch;
		std::vector<size_t>         g_SortIndexScratch;
		std::vector<std::pair<uint32_t, Renderer2D::InstanceContributor>> g_InstanceContributors;
		uint32_t g_NextInstanceContributorToken = 1;

		struct StaticSpriteEntry {
			EntityHandle Entity = entt::null;
			AABB Bounds{};
			uint32_t DrawIndex = 0;
		};

		struct StaticSpriteGrid {
			uint64_t Version = 0;
			float CellSize = 512.0f;
			std::vector<StaticSpriteEntry> Entries;
			std::unordered_map<uint32_t, uint32_t> DrawIndices;
			std::unordered_map<uint64_t, std::vector<uint32_t>> Cells;
			std::vector<uint32_t> OverflowEntries;
			std::vector<uint32_t> QueryMarks;
			uint32_t QueryStamp = 1;
		};

		std::unordered_map<const Scene*, StaticSpriteGrid> g_StaticSpriteGrids;

		// Persistent GPU resources — survive across RenderSceneWithVP calls
		// inside a frame and across frames. Reset on Shutdown.
		wgpu::Buffer g_InstanceBuffer;
		uint32_t     g_InstanceBufferCapacity = 0;  // measured in SpriteInstance units

		wgpu::Buffer g_UniformBuffer;

		// Bind groups are immutable in WebGPU — cache sampler+view alongside the group so SetFilter/hot-reload can detect staleness and rebuild.
		struct BindGroupKey {
			uint64_t TexturePoolId = 0;
			uint64_t MaskPoolId = 0;

			bool operator==(const BindGroupKey& other) const {
				return TexturePoolId == other.TexturePoolId
					&& MaskPoolId == other.MaskPoolId;
			}
		};

		struct BindGroupKeyHash {
			size_t operator()(const BindGroupKey& key) const noexcept {
				const uint64_t mixed = key.TexturePoolId
					^ (key.MaskPoolId + 0x9e3779b97f4a7c15ull
						+ (key.TexturePoolId << 6)
						+ (key.TexturePoolId >> 2));
				return static_cast<size_t>(mixed);
			}
		};

		struct CachedBindGroup {
			wgpu::BindGroup     Group;
			wgpu::Sampler       Sampler;
			wgpu::TextureView   View;
			wgpu::Sampler       MaskSampler;
			wgpu::TextureView   MaskView;
		};
		std::unordered_map<BindGroupKey, CachedBindGroup, BindGroupKeyHash> g_BindGroupCache;

		// Token returned from TextureManager::AddDestroyListener so
		// Shutdown can unregister cleanly. 0 = no listener installed.
		uint32_t g_TextureDestroyListenerToken = 0;

		void OnSpriteRendererDestroyed(entt::registry& registry, EntityHandle entity) {
			if (registry.all_of<DynamicRenderData>(entity)) {
				registry.remove<DynamicRenderData>(entity);
			}
		}

		std::unordered_set<entt::registry*> g_DynamicRenderDataObservedRegistries;

		// alignas(64)+pad keeps each slot on its own cache line, preventing false sharing across workers.
		struct alignas(64) WorkerCollectScratch {
			std::vector<Instance44> Instances;
			char Padding[64];
		};
		constexpr size_t k_MaxWorkers = 64;
		// k_CallerSlot: Job::Wait() work-steals so the render thread (GetWorkerIndex()==-1) runs chunks inline
		// concurrently with real workers — it MUST use its own slot or it races worker 0's vector (heap corruption).
		constexpr size_t k_CallerSlot = k_MaxWorkers;
		std::array<WorkerCollectScratch, k_MaxWorkers + 1> g_WorkerScratch;

		constexpr size_t k_ParallelCollectMinCandidates = 4096;

		// Persistent candidate list filled by the serial pre-pass below.
		// (entity, drawIndex) tuples so the parallel pass doesn't have to
		// re-iterate the EnTT group or recompute the static-grid override.
		struct DynamicCandidate {
			EntityHandle Entity;
			uint32_t     DrawIndex;
		};
		std::vector<DynamicCandidate> g_DynamicCandidates;

		bool Vec2ExactEqual(const Vec2& a, const Vec2& b) {
			return a.x == b.x && a.y == b.y;
		}

		AABB CreateQuadAABB(const Vec2& position, const Vec2& scale, float rotation) {
			if (rotation == 0.0f) {
				const Vec2 halfExtents{ scale.x * 0.5f, scale.y * 0.5f };
				return { position - halfExtents, position + halfExtents };
			}
			if (AABB::IsAxisAligned(rotation)) {
				return AABB::Create(position, Vec2(scale.x * 0.5f, scale.y * 0.5f));
			}
			return AABB::Create(position, scale * 0.5f, Degrees(rotation));
		}

		const AABB& GetStaticSpriteAABB(entt::registry& registry,
			EntityHandle entity,
			const Transform2DComponent& transform)
		{
			StaticRenderData* cache = registry.try_get<StaticRenderData>(entity);
			if (!cache) {
				cache = &registry.emplace<StaticRenderData>(entity);
			}

			if (!cache->Valid
				|| !Vec2ExactEqual(cache->CachedPosition, transform.Position)
				|| !Vec2ExactEqual(cache->CachedScale, transform.Scale)
				|| cache->CachedRotation != transform.Rotation) {
				cache->CachedAABB = CreateQuadAABB(transform.Position, transform.Scale, transform.Rotation);
				cache->CachedPosition = transform.Position;
				cache->CachedScale = transform.Scale;
				cache->CachedRotation = transform.Rotation;
				cache->Valid = true;
			}

			return cache->CachedAABB;
		}

		const AABB& GetDynamicSpriteAABB(entt::registry& registry,
			EntityHandle entity,
			const Transform2DComponent& transform)
		{
			DynamicRenderData* cache = registry.try_get<DynamicRenderData>(entity);
			if (!cache) {
				cache = &registry.emplace<DynamicRenderData>(entity);
			}

			if (!cache->Valid
				|| !Vec2ExactEqual(cache->CachedPosition, transform.Position)
				|| !Vec2ExactEqual(cache->CachedScale, transform.Scale)
				|| cache->CachedRotation != transform.Rotation) {
				cache->CachedAABB = CreateQuadAABB(transform.Position, transform.Scale, transform.Rotation);
				cache->CachedPosition = transform.Position;
				cache->CachedScale = transform.Scale;
				cache->CachedRotation = transform.Rotation;
				cache->Valid = true;
			}

			return cache->CachedAABB;
		}

		AABB GetSpriteAABB(entt::registry& registry,
			EntityHandle entity,
			const Transform2DComponent& transform)
		{
			if (registry.all_of<StaticTag>(entity)) {
				return GetStaticSpriteAABB(registry, entity, transform);
			}
			return GetDynamicSpriteAABB(registry, entity, transform);
		}

		bool IsFinite(const AABB& bounds) {
			return std::isfinite(bounds.Min.x)
				&& std::isfinite(bounds.Min.y)
				&& std::isfinite(bounds.Max.x)
				&& std::isfinite(bounds.Max.y);
		}

		int32_t CellCoord(float value, float cellSize) {
			const float scaled = value / cellSize;
			if (scaled <= static_cast<float>(std::numeric_limits<int32_t>::min())) {
				return std::numeric_limits<int32_t>::min();
			}
			if (scaled >= static_cast<float>(std::numeric_limits<int32_t>::max())) {
				return std::numeric_limits<int32_t>::max();
			}
			return static_cast<int32_t>(std::floor(scaled));
		}

		uint64_t CellKey(int32_t x, int32_t y) {
			return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32)
				| static_cast<uint32_t>(y);
		}

		void AddStaticEntryToGrid(StaticSpriteGrid& grid, uint32_t entryIndex) {
			const AABB& bounds = grid.Entries[entryIndex].Bounds;
			if (!IsFinite(bounds)) {
				grid.OverflowEntries.push_back(entryIndex);
				return;
			}

			const int32_t minX = CellCoord(bounds.Min.x, grid.CellSize);
			const int32_t maxX = CellCoord(bounds.Max.x, grid.CellSize);
			const int32_t minY = CellCoord(bounds.Min.y, grid.CellSize);
			const int32_t maxY = CellCoord(bounds.Max.y, grid.CellSize);
			if (maxX < minX || maxY < minY) {
				grid.OverflowEntries.push_back(entryIndex);
				return;
			}

			const int64_t cellsX = static_cast<int64_t>(maxX) - static_cast<int64_t>(minX) + 1;
			const int64_t cellsY = static_cast<int64_t>(maxY) - static_cast<int64_t>(minY) + 1;
			constexpr int64_t k_MaxCellsPerSprite = 256;
			if (cellsX <= 0 || cellsY <= 0 || cellsX * cellsY > k_MaxCellsPerSprite) {
				grid.OverflowEntries.push_back(entryIndex);
				return;
			}

			for (int32_t y = minY; y <= maxY; ++y) {
				for (int32_t x = minX; x <= maxX; ++x) {
					grid.Cells[CellKey(x, y)].push_back(entryIndex);
				}
			}
		}

		void RebuildStaticSpriteGrid(Scene& scene, StaticSpriteGrid& grid) {
			grid.Version = scene.GetStaticRenderDataVersion();
			grid.Entries.clear();
			grid.DrawIndices.clear();
			grid.Cells.clear();
			grid.OverflowEntries.clear();
			grid.QueryMarks.clear();
			grid.QueryStamp = 1;

			auto& registry = scene.GetRegistry();
			auto view = registry.view<Transform2DComponent, SpriteRendererComponent>(entt::exclude<DisabledTag>);
			grid.Entries.reserve(registry.view<StaticTag>().size());
			grid.DrawIndices.reserve(registry.view<SpriteRendererComponent>().size());

			uint32_t drawIndex = 0;
			for (auto entity : view) {
				const uint32_t currentDrawIndex = drawIndex++;
				grid.DrawIndices[static_cast<uint32_t>(entity)] = currentDrawIndex;
				if (!registry.all_of<StaticTag>(entity)) {
					continue;
				}

				const auto& transform = view.get<Transform2DComponent>(entity);
				const uint32_t entryIndex = static_cast<uint32_t>(grid.Entries.size());
				grid.Entries.push_back(StaticSpriteEntry{
					entity,
					GetStaticSpriteAABB(registry, entity, transform),
					currentDrawIndex
				});
				AddStaticEntryToGrid(grid, entryIndex);
			}

			grid.QueryMarks.assign(grid.Entries.size(), 0);
		}

		StaticSpriteGrid& ResolveStaticSpriteGrid(Scene& scene) {
			StaticSpriteGrid& grid = g_StaticSpriteGrids[&scene];
			if (grid.Version != scene.GetStaticRenderDataVersion()) {
				RebuildStaticSpriteGrid(scene, grid);
			}
			return grid;
		}

		inline void ApplySpriteSliceTo(Instance44& dst,
			const SpriteRendererComponent& sprite)
		{
			Texture2D* tex = TextureManager::GetTexture(sprite.TextureHandle);
			if (!tex) return;
			// Resolve even when the name is empty: the texture may carry a
			// per-texture crop that applies to the whole "single sprite".
			dst.UvRect = ResolveSpriteUVRect(sprite.TextureAssetId,
				sprite.SpriteName,
				static_cast<int>(tex->GetWidth()),
				static_cast<int>(tex->GetHeight()));
		}

		void AppendStaticSpriteInstance(Scene& scene,
			StaticSpriteGrid& grid,
			uint32_t entryIndex,
			const AABB& viewportAABB,
			std::vector<Instance44>& outInstances)
		{
			if (entryIndex >= grid.Entries.size() || entryIndex >= grid.QueryMarks.size()) return;
			if (grid.QueryMarks[entryIndex] == grid.QueryStamp) return;
			grid.QueryMarks[entryIndex] = grid.QueryStamp;

			const StaticSpriteEntry& entry = grid.Entries[entryIndex];
			if (!AABB::Intersects(viewportAABB, entry.Bounds)) {
				return;
			}

			auto& registry = scene.GetRegistry();
			if (!registry.valid(entry.Entity)
				|| !registry.all_of<Transform2DComponent, SpriteRendererComponent, StaticTag>(entry.Entity)
				|| registry.all_of<DisabledTag>(entry.Entity)) {
				scene.MarkStaticRenderDataDirty();
				return;
			}

			const auto& transform = registry.get<Transform2DComponent>(entry.Entity);
			const auto& sprite = registry.get<SpriteRendererComponent>(entry.Entity);
			outInstances.emplace_back(
				Vec2{ transform.Position.x, transform.Position.y },
				Vec2{ transform.Scale.x, transform.Scale.y },
				transform.Rotation,
				sprite.Color,
				sprite.TextureHandle,
				sprite.SortingOrder,
				sprite.SortingLayer,
				entry.DrawIndex);
			ApplySpriteSliceTo(outInstances.back(), sprite);
		}

		void CollectStaticSpriteInstances(Scene& scene,
			StaticSpriteGrid& grid,
			const AABB& viewportAABB,
			std::vector<Instance44>& outInstances)
		{
			if (grid.Entries.empty()) {
				return;
			}

			++grid.QueryStamp;
			if (grid.QueryStamp == 0) {
				std::fill(grid.QueryMarks.begin(), grid.QueryMarks.end(), 0);
				grid.QueryStamp = 1;
			}

			if (!IsFinite(viewportAABB)) {
				for (uint32_t i = 0; i < static_cast<uint32_t>(grid.Entries.size()); ++i) {
					AppendStaticSpriteInstance(scene, grid, i, viewportAABB, outInstances);
				}
				return;
			}

			const int32_t minX = CellCoord(viewportAABB.Min.x, grid.CellSize);
			const int32_t maxX = CellCoord(viewportAABB.Max.x, grid.CellSize);
			const int32_t minY = CellCoord(viewportAABB.Min.y, grid.CellSize);
			const int32_t maxY = CellCoord(viewportAABB.Max.y, grid.CellSize);

			const int64_t cellsX = static_cast<int64_t>(maxX) - static_cast<int64_t>(minX) + 1;
			const int64_t cellsY = static_cast<int64_t>(maxY) - static_cast<int64_t>(minY) + 1;
			constexpr int64_t k_MaxQueryCells = 4096;
			if (maxX >= minX && maxY >= minY && cellsX > 0 && cellsY > 0 && cellsX * cellsY <= k_MaxQueryCells) {
				for (int32_t y = minY; y <= maxY; ++y) {
					for (int32_t x = minX; x <= maxX; ++x) {
						auto it = grid.Cells.find(CellKey(x, y));
						if (it == grid.Cells.end()) continue;
						for (uint32_t entryIndex : it->second) {
							AppendStaticSpriteInstance(scene, grid, entryIndex, viewportAABB, outInstances);
						}
					}
				}
			}
			else {
				for (uint32_t i = 0; i < static_cast<uint32_t>(grid.Entries.size()); ++i) {
					AppendStaticSpriteInstance(scene, grid, i, viewportAABB, outInstances);
				}
			}

			for (uint32_t entryIndex : grid.OverflowEntries) {
				AppendStaticSpriteInstance(scene, grid, entryIndex, viewportAABB, outInstances);
			}
		}

		void EnsureDynamicCacheObserver(entt::registry& registry) {
			if (g_DynamicRenderDataObservedRegistries.insert(&registry).second) {
				registry.on_destroy<SpriteRendererComponent>().connect<&OnSpriteRendererDestroyed>();
			}
		}

		// ── CPU collect path ────────────────────────────────────────────────
		size_t CollectSpriteInstances(Scene& scene,
			const AABB& viewportAABB,
			std::vector<Instance44>& outInstances,
			std::vector<TextureHandle>& outTextures)
		{
			INDEX_PROFILE_SCOPE("Renderer2D.Collect");
			outInstances.clear();
			outTextures.clear();
			auto& registry = scene.GetRegistry();
			EnsureDynamicCacheObserver(registry);
			StaticSpriteGrid* staticGrid = nullptr;
			if (registry.view<StaticTag>().size() > 0) {
				staticGrid = &ResolveStaticSpriteGrid(scene);
			}

			// Owning group: no other site groups these two components — overlapping ownership would assert at startup.
			auto group = registry.group<Transform2DComponent, SpriteRendererComponent>(
				entt::get<>, entt::exclude<DisabledTag, StaticTag>);
			const size_t dynamicHint = group.size();
			const size_t staticHint = staticGrid ? staticGrid->Entries.size() : 0;
			outInstances.reserve(dynamicHint + staticHint);

			g_DynamicCandidates.clear();
			g_DynamicCandidates.reserve(dynamicHint);
			uint32_t dynamicDrawIndex = 0;
			for (auto entity : group) {
				uint32_t currentDrawIndex = dynamicDrawIndex++;
				if (staticGrid) {
					auto drawIndexIt = staticGrid->DrawIndices.find(static_cast<uint32_t>(entity));
					if (drawIndexIt != staticGrid->DrawIndices.end()) {
						currentDrawIndex = drawIndexIt->second;
					}
				}
				// Pre-emplace to avoid sparse-set resize races in the parallel pass.
				if (!registry.all_of<DynamicRenderData>(entity)) {
					registry.emplace<DynamicRenderData>(entity);
				}
				g_DynamicCandidates.push_back({ entity, currentDrawIndex });
			}

			const size_t candidateCount = g_DynamicCandidates.size();
			const bool useParallel = candidateCount >= k_ParallelCollectMinCandidates
				&& JobSystem::IsInitialized()
				&& JobSystem::GetWorkerCount() > 1;

			if (useParallel) {
				for (auto& ws : g_WorkerScratch) {
					ws.Instances.clear();
				}

				ParallelFor(0, candidateCount, [&](size_t lo, size_t hi) {
					int wi = JobSystem::GetWorkerIndex();
					if (wi < 0 || wi >= static_cast<int>(k_MaxWorkers)) {
						wi = static_cast<int>(k_CallerSlot); // render thread work-steals inline — must use its own slot
					}
					auto& localInst = g_WorkerScratch[wi].Instances;

					for (size_t i = lo; i < hi; ++i) {
						const auto& cand = g_DynamicCandidates[i];
						const auto& t = group.get<Transform2DComponent>(cand.Entity);
						const auto& spr = group.get<SpriteRendererComponent>(cand.Entity);

						DynamicRenderData& cache = registry.get<DynamicRenderData>(cand.Entity);
						if (!cache.Valid
							|| !Vec2ExactEqual(cache.CachedPosition, t.Position)
							|| !Vec2ExactEqual(cache.CachedScale, t.Scale)
							|| cache.CachedRotation != t.Rotation) {
							cache.CachedAABB = CreateQuadAABB(t.Position, t.Scale, t.Rotation);
							cache.CachedPosition = t.Position;
							cache.CachedScale    = t.Scale;
							cache.CachedRotation = t.Rotation;
							cache.Valid          = true;
						}

						if (!AABB::Intersects(viewportAABB, cache.CachedAABB)) {
							continue;
						}

						localInst.emplace_back(
							Vec2{ t.Position.x, t.Position.y },
							Vec2{ t.Scale.x, t.Scale.y },
							t.Rotation,
							spr.Color,
							spr.TextureHandle,
							spr.SortingOrder,
							spr.SortingLayer,
							cand.DrawIndex);
						ApplySpriteSliceTo(localInst.back(), spr);
					}
				}, /*grainSize*/ 2048);

				for (auto& ws : g_WorkerScratch) {
					if (ws.Instances.empty()) continue;
					outInstances.insert(outInstances.end(),
						ws.Instances.begin(), ws.Instances.end());
				}
			} else {
				// Small N: serial path, no job overhead.
				for (const auto& cand : g_DynamicCandidates) {
					const auto& t = group.get<Transform2DComponent>(cand.Entity);
					const auto& s = group.get<SpriteRendererComponent>(cand.Entity);
					const AABB& bounds = GetDynamicSpriteAABB(registry, cand.Entity, t);
					if (!AABB::Intersects(viewportAABB, bounds)) {
						continue;
					}
					outInstances.emplace_back(
						Vec2{ t.Position.x, t.Position.y },
						Vec2{ t.Scale.x, t.Scale.y },
						t.Rotation,
						s.Color,
						s.TextureHandle,
						s.SortingOrder,
						s.SortingLayer,
						cand.DrawIndex);
					ApplySpriteSliceTo(outInstances.back(), s);
				}
			}

			if (staticGrid) {
				CollectStaticSpriteInstances(scene, *staticGrid, viewportAABB, outInstances);
			}

			auto particleView = scene.GetRegistry().view<ParticleSystem2DComponent>(entt::exclude<DisabledTag>);
			for (auto entity : particleView) {
				const auto& particleSystem = particleView.get<ParticleSystem2DComponent>(entity);
				const TextureHandle texture = particleSystem.GetTextureHandle();
				for (const auto& particle : particleSystem.GetParticles()) {
					Vec2 position = particle.Transform.Position;
					Vec2 scale = particle.Transform.Scale;
					float rotation = particle.Transform.Rotation;
					if (particleSystem.EmissionSettings.EmissionSpace == ParticleSystem2DComponent::Space::Local) {
						const auto& emitterTransform = particleSystem.GetTransform2D();
						position = emitterTransform.TransformPoint(position);
						scale = Vec2{
							scale.x * emitterTransform.Scale.x,
							scale.y * emitterTransform.Scale.y
						};
						rotation += emitterTransform.Rotation;
					}
					if (!AABB::Intersects(viewportAABB, CreateQuadAABB(position, scale, rotation))) {
						continue;
					}
					outInstances.emplace_back(
						position,
						scale,
						rotation,
						particle.Color,
						texture,
						particleSystem.RenderingSettings.SortingOrder,
						particleSystem.RenderingSettings.SortingLayer,
						static_cast<std::uint32_t>(outInstances.size()));
				}
			}

			for (const auto& [token, contributor] : g_InstanceContributors) {
				(void)token;
				if (contributor) {
					contributor(scene, viewportAABB, outInstances);
				}
			}

			outTextures.reserve(outInstances.size());
			for (const Instance44& instance : outInstances) {
				outTextures.push_back(instance.TextureHandle);
			}
			return outInstances.size();
		}

		inline uint64_t PackSortKey(const Instance44& inst) {
			// XOR 0x8000 biases signed→unsigned for sorting; addition would be signed-overflow UB near INT16_MAX.
			const uint16_t orderBiased = static_cast<uint16_t>(inst.SortingOrder) ^ uint16_t(0x8000);
			return (static_cast<uint64_t>(inst.SortingLayer) << 56)
				 | (static_cast<uint64_t>(orderBiased)       << 32)
				 | static_cast<uint64_t>(inst.DrawIndex);
		}

		std::vector<uint64_t> g_RadixKeys;
		std::vector<size_t>   g_RadixIndicesB;  // g_SortIndexScratch is the A buffer
		constexpr size_t k_RadixSortMinElements = 4096;

		void SortInstancesInPlace(size_t n) {
			if (n < 2) return;
			INDEX_PROFILE_SCOPE("Renderer2D.Sort");
			auto sortLess = [](const Instance44& a, const Instance44& b) {
				if (a.SortingLayer != b.SortingLayer) return a.SortingLayer < b.SortingLayer;
				if (a.SortingOrder != b.SortingOrder) return a.SortingOrder < b.SortingOrder;
				return a.DrawIndex < b.DrawIndex;
			};

			bool alreadySorted = true;
			for (size_t k = 1; k < n; ++k) {
				if (sortLess(g_InstancesScratch[k], g_InstancesScratch[k - 1])) {
					alreadySorted = false;
					break;
				}
			}
			if (alreadySorted) return;

			g_SortIndexScratch.resize(n);
			for (size_t k = 0; k < n; ++k) g_SortIndexScratch[k] = k;

			if (n < k_RadixSortMinElements) {
				// Small N: std::sort beats radix's 8-pass linear cost.
				std::sort(g_SortIndexScratch.begin(), g_SortIndexScratch.end(),
					[](size_t a, size_t b) {
						const auto& ia = g_InstancesScratch[a];
						const auto& ib = g_InstancesScratch[b];
						if (ia.SortingLayer != ib.SortingLayer) return ia.SortingLayer < ib.SortingLayer;
						if (ia.SortingOrder != ib.SortingOrder) return ia.SortingOrder < ib.SortingOrder;
						return ia.DrawIndex < ib.DrawIndex;
					});
			} else {
				g_RadixKeys.resize(n);
				g_RadixIndicesB.resize(n);
				for (size_t k = 0; k < n; ++k) {
					g_RadixKeys[k] = PackSortKey(g_InstancesScratch[k]);
				}

				std::vector<size_t>* src = &g_SortIndexScratch;
				std::vector<size_t>* dst = &g_RadixIndicesB;

				for (int byteIdx = 0; byteIdx < 8; ++byteIdx) {
					const int shift = byteIdx * 8;
					uint32_t counts[256] = {};

					for (size_t k = 0; k < n; ++k) {
						const uint8_t bin = static_cast<uint8_t>((g_RadixKeys[(*src)[k]] >> shift) & 0xFF);
						++counts[bin];
					}

					// Exclusive prefix sum -> bin-start offsets.
					uint32_t sum = 0;
					for (int b = 0; b < 256; ++b) {
						const uint32_t c = counts[b];
						counts[b] = sum;
						sum += c;
					}

					for (size_t k = 0; k < n; ++k) {
						const size_t srcIdx = (*src)[k];
						const uint8_t bin = static_cast<uint8_t>((g_RadixKeys[srcIdx] >> shift) & 0xFF);
						(*dst)[counts[bin]++] = srcIdx;
					}

					std::swap(src, dst);
				}

				if (src != &g_SortIndexScratch) {
					g_SortIndexScratch = *src;
				}
			}

			// READ-direction permutation (sorted[k]=src[idx[k]]): walk cycles — do NOT use swap-as-you-go (WRITE direction) which would mis-sort.
			for (size_t i = 0; i < n; ++i) {
				if (g_SortIndexScratch[i] == i) continue;
				Instance44    holdInst = g_InstancesScratch[i];
				TextureHandle holdTex  = g_TexturesScratch[i];
				size_t j = i;
				while (g_SortIndexScratch[j] != i) {
					const size_t src = g_SortIndexScratch[j];
					g_InstancesScratch[j] = g_InstancesScratch[src];
					g_TexturesScratch[j]  = g_TexturesScratch[src];
					g_SortIndexScratch[j] = j; // mark settled
					j = src;
				}
				g_InstancesScratch[j] = holdInst;
				g_TexturesScratch[j]  = holdTex;
				g_SortIndexScratch[j] = j;
			}
		}

		// ── GPU resource helpers ────────────────────────────────────────────

		bool EnsureUniformBuffer(wgpu::Device device) {
			if (g_UniformBuffer) return true;
			wgpu::BufferDescriptor desc{};
			desc.size  = 64;  // mat4x4<f32>
			desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
			desc.label = "renderer2d-viewproj-ubo";
			g_UniformBuffer = device.CreateBuffer(&desc);
			if (!g_UniformBuffer) {
				IDX_CORE_ERROR_TAG("Renderer2D", "Failed to create uniform buffer");
				return false;
			}
			return true;
		}

		bool EnsureInstanceBuffer(wgpu::Device device, uint32_t neededInstances) {
			if (g_InstanceBufferCapacity >= neededInstances && g_InstanceBuffer) return true;

			uint32_t newCapacity = g_InstanceBufferCapacity > 0 ? g_InstanceBufferCapacity : 256;
			while (newCapacity < neededInstances) newCapacity *= 2;

			wgpu::BufferDescriptor desc{};
			desc.size  = static_cast<uint64_t>(newCapacity) * sizeof(SpriteInstance);
			desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
			desc.label = "renderer2d-instance-vbo";
			wgpu::Buffer buf = device.CreateBuffer(&desc);
			if (!buf) {
				IDX_CORE_ERROR_TAG("Renderer2D",
					"Failed to create instance buffer (cap={})", newCapacity);
				return false;
			}
			g_InstanceBuffer         = std::move(buf);
			g_InstanceBufferCapacity = newCapacity;
			return true;
		}

		wgpu::BindGroup ResolveBindGroup(wgpu::Device device,
			TextureHandle handle,
			TextureHandle maskHandle)
		{
			Texture2D* tex = TextureManager::GetTexture(handle);
			Texture2D* maskTex = TextureManager::GetTexture(maskHandle);
			if (!tex || !tex->IsValid() || !maskTex || !maskTex->IsValid()) return nullptr;
			const uint64_t poolId = tex->GetHandle();
			const uint64_t maskPoolId = maskTex->GetHandle();

			const auto lookup = WebGPUBackend::LookupTexture2D(poolId);
			const auto maskLookup = WebGPUBackend::LookupTexture2D(maskPoolId);
			if (!lookup.Valid || !lookup.View || !lookup.Sampler
				|| !maskLookup.Valid || !maskLookup.View || !maskLookup.Sampler) {
				return nullptr;
			}

			const BindGroupKey key{ poolId, maskPoolId };
			auto cacheIt = g_BindGroupCache.find(key);
			if (cacheIt != g_BindGroupCache.end()) {
				const CachedBindGroup& cached = cacheIt->second;
				if (cached.Sampler.Get() == lookup.Sampler.Get()
					&& cached.View.Get() == lookup.View.Get()
					&& cached.MaskSampler.Get() == maskLookup.Sampler.Get()
					&& cached.MaskView.Get() == maskLookup.View.Get()) {
					return cached.Group;
				}
				// Sampler or view rebuilt under us (filter change, hot
				// reload) — drop the stale group so we recreate one
				// against the current handles.
				g_BindGroupCache.erase(cacheIt);
			}

			wgpu::BindGroupEntry entries[5] = {};
			entries[0].binding = 0;
			entries[0].buffer  = g_UniformBuffer;
			entries[0].offset  = 0;
			entries[0].size    = 64;
			entries[1].binding     = 1;
			entries[1].textureView = lookup.View;
			entries[2].binding = 2;
			entries[2].sampler = lookup.Sampler;
			entries[3].binding     = 3;
			entries[3].textureView = maskLookup.View;
			entries[4].binding = 4;
			entries[4].sampler = maskLookup.Sampler;

			wgpu::BindGroupDescriptor desc{};
			desc.layout     = WebGPUSpriteResources::GetBindGroupLayout();
			desc.entryCount = 5;
			desc.entries    = entries;
			desc.label      = "renderer2d-sprite-bindgroup";

			wgpu::BindGroup bg = device.CreateBindGroup(&desc);
			if (!bg) return nullptr;
			g_BindGroupCache.emplace(key, CachedBindGroup{
				bg, lookup.Sampler, lookup.View, maskLookup.Sampler, maskLookup.View });
			return bg;
		}
	}

	// ── Renderer2D ──────────────────────────────────────────────────────────

	Renderer2D::Renderer2D() = default;
	Renderer2D::~Renderer2D() = default;

	void Renderer2D::Initialize() {
		WebGPUSpriteResources::Acquire();
		// Evict cache entry before GPU teardown — stale group holding freed TextureView causes use-after-free on next draw.
		if (g_TextureDestroyListenerToken == 0) {
			g_TextureDestroyListenerToken = TextureManager::AddDestroyListener(
				[](TextureHandle handle) {
					Texture2D* tex = TextureManager::GetTexture(handle);
					if (!tex) return;
					g_BindGroupCache.clear();
				});
		}

#ifdef INDEX_PROFILER_ENABLED
		m_GpuTimer = std::make_unique<GpuTimer>();
		m_GpuTimer->Initialize();
#endif

		if (!m_PostProcessor.Initialize()) {
			IDX_CORE_WARN_TAG("Renderer2D",
				"PostProcessor::Initialize failed — scene will render direct to caller (no PP)");
		}

		m_IsInitialized = true;
	}

	void Renderer2D::Shutdown() {
		if (!m_IsInitialized) return;
		WebGPUSpriteResources::Release();

		if (g_TextureDestroyListenerToken != 0) {
			TextureManager::RemoveDestroyListener(g_TextureDestroyListenerToken);
			g_TextureDestroyListenerToken = 0;
		}

#ifdef INDEX_PROFILER_ENABLED
		// MUST shut down before WebGPUApi::Shutdown — QuerySet + readback buffers hold Dawn handles.
		if (m_GpuTimer) {
			m_GpuTimer->Shutdown();
			m_GpuTimer.reset();
		}
#endif

		// MUST precede WebGPUApi::Shutdown — FBO and pipelines hold Dawn handles.
		m_PostProcessor.Shutdown();
		m_SceneFbo.Destroy();

		g_BindGroupCache.clear();
		g_StaticSpriteGrids.clear();
		g_DynamicRenderDataObservedRegistries.clear();
		g_InstanceBuffer         = nullptr;
		g_InstanceBufferCapacity = 0;
		g_UniformBuffer          = nullptr;

		m_IsInitialized = false;
	}

	void Renderer2D::ClearSceneCache(const Scene* scene) {
		if (!scene) {
			g_StaticSpriteGrids.clear();
			// Also clear: a dangling ptr at the same address would silently skip on_destroy connect for the new registry.
			g_DynamicRenderDataObservedRegistries.clear();
			return;
		}
		g_StaticSpriteGrids.erase(scene);
		g_DynamicRenderDataObservedRegistries.erase(
			const_cast<entt::registry*>(&scene->GetRegistry()));
	}

	void Renderer2D::BeginFrame() {
		m_DrawCallsCount = 0;
		// NOT reset: editor throttles Game View so RenderSceneWithVP runs on a subset of frames; resetting here flickers the overlay to 0.
		m_RenderLoopDuration = 0.0f;
		// GpuTimer::OnFrameStart (deferred MapAsync) MUST NOT run here — Layer.OnPreRender calls RenderSceneWithVP before BeginFrame; MapAsync here would race the same-frame Submit.
		if (m_SkipBeginFrameRender) return;
		if (!m_SceneProvider) return;

		Camera2DComponent* cam = Camera2DComponent::Main();
		if (!cam || !cam->IsValid()) return;

		RenderApi::BindDefaultFramebuffer();

		// Clear swap chain to BLACK for aspect-locked letterbox bars before the sub-rect blit; restore camera clear color immediately after.
		Viewport* mainViewport = Window::GetMainViewport();
		const bool hasLetterbox = mainViewport && mainViewport->HasLetterbox();
		if (hasLetterbox) {
			RenderApi::SetClearColor(Color{ 0.0f, 0.0f, 0.0f, 1.0f });
			RenderApi::Clear(ClearFlags::Color | ClearFlags::Depth);
			// Restore camera clear color so the scene FBO (not just the letterbox bars) uses it.
			RenderApi::SetClearColor(cam->GetClearColor());
		} else {
			// Standard path: clear swap chain to camera color.
			RenderApi::SetClearColor(cam->GetClearColor());
			RenderApi::Clear(ClearFlags::Color | ClearFlags::Depth);
		}

		cam->UpdateViewport();

		const glm::mat4 vp = cam->GetViewProjectionMatrix();
		const AABB viewAABB = cam->GetViewportAABB();
		m_SceneProvider([&](Scene& scene) {
			RenderSceneWithVP(scene, vp, viewAABB);
		});
	}

	void Renderer2D::EndFrame() {
		INDEX_PROFILE_VALUE("Batches", static_cast<float>(m_DrawCallsCount));
		INDEX_PROFILE_VALUE("Rendered Sprites", static_cast<float>(m_RenderedInstancesCount));

#ifdef INDEX_PROFILER_ENABLED
		if (m_GpuTimer) {
			m_GpuTimer->PollAndPublish();
		}
#endif
	}

	void Renderer2D::OnAfterPresent() {
#ifdef INDEX_PROFILER_ENABLED
		// MUST run here (post-Present): MapAsync earlier races the same-frame CopyBufferToBuffer; Dawn rejects the command buffer.
		if (m_GpuTimer) {
			m_GpuTimer->OnFrameStart();
		}
#endif
	}

	void Renderer2D::RenderScene(Scene& /*scene*/) {
	}

	void Renderer2D::RenderSceneWithVP(Scene& scene,
		const glm::mat4& vp, const AABB& viewportAABB)
	{
		if (!m_IsInitialized) return;
		if (!WebGPUSpriteResources::IsReady()) return;

		const auto renderStart = std::chrono::steady_clock::now();
		auto finishTiming = [&]() {
			m_RenderLoopDuration = std::chrono::duration<float, std::milli>(
				std::chrono::steady_clock::now() - renderStart).count();
			Profiler::PushSample("Rendering", m_RenderLoopDuration);
			};

		const size_t n = CollectSpriteInstances(scene, viewportAABB, g_InstancesScratch, g_TexturesScratch);
		m_RenderedInstancesCount = n;
		if (n > 0) {
			SortInstancesInPlace(n);
		}

		const auto callerSnap = WebGPUBackend::SaveBoundTarget();
		const auto callerInfo = WebGPUBackend::BeginRenderToCurrentTarget();
		if (!callerInfo.Valid) {
			finishTiming();
			return;
		}

		// runEffects: execute effect passes. usePostProcess: route through intermediate FBO (needed for effects OR aspect-locked letterbox).
		// These MUST stay separate: collapsing to one flag silently ignores "disable PP on camera" in aspect-locked standalone builds.
		bool runEffects = m_PostProcessor.IsInitialized() && m_PostProcessingEnabled;
		if (runEffects) {
			if (IndexProject* proj = ProjectManager::GetCurrentProject()) {
				runEffects = proj->EnablePostProcessing;
			}
		}
		if (runEffects) {
			if (Camera2DComponent* mainCam = Camera2DComponent::Main()) {
				runEffects = mainCam->IsPostProcessingEnabled();
			}
		}
		bool usePostProcess = runEffects;

		Viewport* mvp = Window::GetMainViewport();
		const bool aspectLocked = callerInfo.IsSwapChain && mvp && mvp->HasLetterbox()
			&& m_PostProcessor.IsInitialized();
		if (aspectLocked) {
			usePostProcess = true;
		}

		const int fboWidth  = aspectLocked
			? mvp->GetWidth()
			: static_cast<int>(callerInfo.Width);
		const int fboHeight = aspectLocked
			? mvp->GetHeight()
			: static_cast<int>(callerInfo.Height);

		if (usePostProcess) {
			if (!m_SceneFbo.Recreate(fboWidth, fboHeight, TextureFormat::RGBA16F)) {
				usePostProcess = false;
			}
		}

		if (n == 0 && !usePostProcess) {
			finishTiming();
			return;
		}

		if (usePostProcess) {
			RenderApi::BindFramebuffer(m_SceneFbo);
			RenderApi::SetViewport(0, 0, fboWidth, fboHeight);
			if (RenderApi::GetPolygonMode() == PolygonMode::Wireframe) {
				const Color savedClear = RenderApi::GetClearColor();
				RenderApi::SetClearColor(Color{ 0.0f, 0.0f, 0.0f, 0.0f });
				RenderApi::Clear(ClearFlags::Color | ClearFlags::Depth);
				RenderApi::SetClearColor(savedClear);
			} else {
				RenderApi::Clear(ClearFlags::Color | ClearFlags::Depth);
			}
		}

		// Sprite pass — only when there are sprites to draw. When n == 0
		// with PP enabled we fall through to the PostProcessor below so
		// effects still apply to the cleared intermediate background.
		if (n > 0) {
			auto target = WebGPUBackend::BeginRenderToCurrentTarget();
			if (!target.Valid) {
				if (usePostProcess) WebGPUBackend::RestoreBoundTarget(callerSnap);
				finishTiming();
				return;
			}

			wgpu::Device device = WebGPUBackend::GetDevice();
			wgpu::Queue  queue  = WebGPUBackend::GetQueue();
			if (!device || !queue) {
				finishTiming();
				return;
			}

			const bool wireframePass = RenderApi::GetPolygonMode() == PolygonMode::Wireframe;
			const auto pipelineMode = wireframePass
				? WebGPUSpriteResources::SpritePipelineMode::Wireframe
				: WebGPUSpriteResources::SpritePipelineMode::Filled;
			wgpu::RenderPipeline pipeline = WebGPUSpriteResources::GetSpritePipeline(
				target.ColorFormat, target.HasDepth, pipelineMode);
			if (!pipeline) {
				IDX_CORE_WARN_TAG("Renderer2D",
					"No pipeline for color-format {} (hasDepth={}) — skipping submit",
					static_cast<int>(target.ColorFormat), target.HasDepth);
				finishTiming();
				return;
			}

			{
				INDEX_PROFILE_SCOPE("Renderer2D.Upload");
				if (!EnsureUniformBuffer(device)) {
					finishTiming();
					return;
				}
				queue.WriteBuffer(g_UniformBuffer, 0, glm::value_ptr(vp), 64);

				if (!EnsureInstanceBuffer(device, static_cast<uint32_t>(n))) {
					finishTiming();
					return;
				}
				g_GpuInstanceScratch.resize(n);
				for (size_t k = 0; k < n; ++k) {
					WebGPUSpriteResources::EncodeInstance44(g_InstancesScratch[k], g_GpuInstanceScratch[k]);
				}
				queue.WriteBuffer(g_InstanceBuffer, 0,
					g_GpuInstanceScratch.data(),
					n * sizeof(SpriteInstance));
			}

			wgpu::CommandEncoder encoder = WebGPUBackend::GetFrameEncoder();
			if (!encoder) {
				finishTiming();
				return;
			}

			INDEX_PROFILE_SCOPE("Renderer2D.Submit");
				wgpu::RenderPassColorAttachment colorAtt{};
			colorAtt.view       = target.ColorView;
			colorAtt.loadOp     = wgpu::LoadOp::Load;
			colorAtt.storeOp    = wgpu::StoreOp::Store;
			colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

			wgpu::RenderPassDepthStencilAttachment depthAtt{};
			if (target.HasDepth) {
				depthAtt.view              = target.DepthView;
				depthAtt.depthLoadOp       = wgpu::LoadOp::Load;
				depthAtt.depthStoreOp      = wgpu::StoreOp::Store;
				depthAtt.stencilLoadOp     = wgpu::LoadOp::Load;
				depthAtt.stencilStoreOp    = wgpu::StoreOp::Store;
			}

			wgpu::RenderPassDescriptor passDesc{};
			passDesc.label                  = "renderer2d-sprites";
			passDesc.colorAttachmentCount   = 1;
			passDesc.colorAttachments       = &colorAtt;
			passDesc.depthStencilAttachment = target.HasDepth ? &depthAtt : nullptr;

#ifdef INDEX_PROFILER_ENABLED
			GpuTimer::FrameSlots gpuTimerSlots;
			wgpu::PassTimestampWrites tsWrites{};
			if (m_GpuTimer) {
				gpuTimerSlots = m_GpuTimer->BeginFrameWrites();
				if (gpuTimerSlots.Valid) {
					tsWrites.querySet                  = gpuTimerSlots.QuerySet;
					tsWrites.beginningOfPassWriteIndex = gpuTimerSlots.BeginningOfPassWriteIndex;
					tsWrites.endOfPassWriteIndex       = gpuTimerSlots.EndOfPassWriteIndex;
					passDesc.timestampWrites = &tsWrites;
				}
			}
#endif

			wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
			WebGPUBackend::ApplyCachedViewportToPass(pass);

			pass.SetPipeline(pipeline);
			pass.SetVertexBuffer(0, WebGPUSpriteResources::GetQuadVertexBuffer());
			pass.SetVertexBuffer(1, g_InstanceBuffer);
			pass.SetIndexBuffer(WebGPUSpriteResources::GetQuadIndexBuffer(pipelineMode),
				wgpu::IndexFormat::Uint16);

			// When HasBindlessTextures() becomes true, replace this loop with a single DrawIndexed(n) — see WebGPUBackend::HasBindlessTextures().
			if (WebGPUBackend::HasBindlessTextures()) {
				// Bindless path is not yet implemented — fallback warning below.
				IDX_CORE_WARN_TAG("Renderer2D",
					"HasBindlessTextures returned true but the bindless submit "
					"path is not implemented. Falling back to per-texture-run.");
			}
			const TextureHandle defaultTexture = TextureManager::GetDefaultTexture(DefaultTexture::Square);
			auto resolveHandle = [&](TextureHandle h) {
				return TextureManager::IsValid(h) ? h : defaultTexture;
			};
			auto resolveMaskHandle = [&](const Instance44& inst) {
				return inst.HasTextureMask && TextureManager::IsValid(inst.MaskTextureHandle)
					? inst.MaskTextureHandle
					: defaultTexture;
			};
			const std::uint32_t indexCount = WebGPUSpriteResources::GetQuadIndexCount(pipelineMode);

			size_t i = 0;
			while (i < n) {
				const TextureHandle runHandle = resolveHandle(g_TexturesScratch[i]);
				const TextureHandle runMaskHandle = resolveMaskHandle(g_InstancesScratch[i]);
				size_t runEnd = i + 1;
				while (runEnd < n
					&& resolveHandle(g_TexturesScratch[runEnd]).index == runHandle.index
					&& resolveHandle(g_TexturesScratch[runEnd]).generation == runHandle.generation
					&& resolveMaskHandle(g_InstancesScratch[runEnd]).index == runMaskHandle.index
					&& resolveMaskHandle(g_InstancesScratch[runEnd]).generation == runMaskHandle.generation)
				{
					++runEnd;
				}
				const uint32_t count = static_cast<uint32_t>(runEnd - i);

				wgpu::BindGroup bg = ResolveBindGroup(device, runHandle, runMaskHandle);
				if (!bg) {
					i = runEnd;
					continue;
				}
				pass.SetBindGroup(0, bg);
				pass.DrawIndexed(/*indexCount=*/indexCount,
					/*instanceCount=*/count,
					/*firstIndex=*/0,
					/*baseVertex=*/0,
					/*firstInstance=*/static_cast<uint32_t>(i));
				++m_DrawCallsCount;

				i = runEnd;
			}

			pass.End();

#ifdef INDEX_PROFILER_ENABLED
			if (m_GpuTimer && gpuTimerSlots.Valid) {
				m_GpuTimer->ResolveCurrentFrame(encoder);
			}
#endif
		}

		if (usePostProcess) {
			// ppSettings=nullptr forces passthrough Blit (no effects) — needed when usePostProcess is on for letterbox but runEffects is off.
			const PostProcessing2DComponent* ppSettings = nullptr;
			if (runEffects) {
				if (Camera2DComponent* mainCam = Camera2DComponent::Main()) {
					if (Scene* camScene = mainCam->GetOwnerScene()) {
						const EntityHandle camEnt = mainCam->GetOwnerEntity();
						if (camEnt != entt::null
							&& camScene->HasComponent<PostProcessing2DComponent>(camEnt))
						{
							ppSettings = &camScene->GetComponent<PostProcessing2DComponent>(camEnt);
						}
					}
				}
			}
			// MUST set viewport to sub-rect BEFORE Run so Blit clips to the letterbox sub-rect and leaves the surrounding bars black.
			uint32_t dstW = callerInfo.Width;
			uint32_t dstH = callerInfo.Height;
			if (aspectLocked) {
				RenderApi::SetViewport(
					mvp->GetOffsetX(), mvp->GetOffsetY(),
					mvp->GetWidth(),   mvp->GetHeight());
				dstW = static_cast<uint32_t>(mvp->GetWidth());
				dstH = static_cast<uint32_t>(mvp->GetHeight());
			}

			// Aspect-locked + effects: RunEffectPass does NOT apply the cached viewport, so route through a sub-rect FBO then Blit (which does).
			// HasEnabledEffect MUST mirror PostProcessor::Run's effect set — add new effects here or they smear in aspect-locked standalone builds.
			auto HasEnabledEffect = [](const PostProcessing2DComponent* s) {
				if (!s) return false;
				return s->Bloom.Enabled
					|| s->ColorGrading.Enabled
					|| s->GaussianBlur.Enabled
					|| s->LensDistortion.Enabled
					|| s->ChromaticAberration.Enabled
					|| s->Vignette.Enabled
					|| s->Grain.Enabled
					|| s->Pixelated.Enabled;
			};
			const bool routeThroughLetterboxFbo = aspectLocked
				&& HasEnabledEffect(ppSettings)
				&& m_LetterboxOutputFbo.Recreate(
					static_cast<int>(dstW), static_cast<int>(dstH),
					TextureFormat::RGBA16F);

			if (routeThroughLetterboxFbo) {
				const auto outLook = WebGPUBackend::LookupFramebufferByFboId(
					m_LetterboxOutputFbo.GetBackendId());
				if (outLook.Valid) {
					// 1) Full effect chain into the sub-rect intermediate.
					m_PostProcessor.Run(m_SceneFbo,
						outLook.ColorView,
						outLook.ColorFormat,
						dstW, dstH,
						ppSettings);
					// 2) Composite intermediate -> swap chain. Blit honours
					//    the cached viewport (sub-rect of swap chain) so
					//    the surround stays black.
					m_PostProcessor.Blit(m_LetterboxOutputFbo,
						callerInfo.ColorView,
						callerInfo.ColorFormat,
						callerInfo.Width,
						callerInfo.Height);
				}
				else {
					m_PostProcessor.Run(m_SceneFbo,
						callerInfo.ColorView,
						callerInfo.ColorFormat,
						dstW, dstH,
						ppSettings);
				}
			}
			else {
				m_PostProcessor.Run(m_SceneFbo,
					callerInfo.ColorView,
					callerInfo.ColorFormat,
					dstW, dstH,
					ppSettings);
			}
			WebGPUBackend::RestoreBoundTarget(callerSnap);
		}
		else {
			// No post-process: the sprite pass drew straight to the target, and unlike
			// the PP branch nothing has flushed yet. Submit now so this scene's
			// WriteBuffer(g_InstanceBuffer)+pass is its own atomic unit. Otherwise the
			// NEXT additive scene's WriteBuffer to the same shared buffer overwrites
			// this scene's instance data before this pass executes — Dawn coalesces all
			// WriteBuffers in a submit ahead of any pass (see FlushFrameCommands). With
			// n==0 && !usePostProcess already early-returned above, reaching here means
			// real instance data was uploaded.
			WebGPUBackend::FlushCommands();
		}

		// Use callerInfo (NOT target): after PP redirect, target is the intermediate FBO, not the swap chain.
		if (callerInfo.IsSwapChain) {
			WebGPUBackend::MarkSwapChainRendered();
		}
		finishTiming();
	}

	void Renderer2D::RenderScenes() {
	}

	void Renderer2D::CollectAndRenderInstances(Scene& /*scene*/,
		const glm::mat4& /*vp*/, const AABB& /*viewportAABB*/)
	{
	}

	uint32_t Renderer2D::RegisterInstanceContributor(InstanceContributor contributor) {
		if (!contributor) return 0;
		const uint32_t token = g_NextInstanceContributorToken++;
		if (g_NextInstanceContributorToken == 0) {
			g_NextInstanceContributorToken = 1;
		}
		g_InstanceContributors.emplace_back(token, std::move(contributor));
		return token;
	}

	void Renderer2D::UnregisterInstanceContributor(uint32_t token) {
		std::erase_if(g_InstanceContributors,
			[token](const auto& entry) { return entry.first == token; });
	}

}  // namespace Index
