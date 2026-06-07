#include "pch.hpp"
#include "Scene/EntityPicker.hpp"

#include "Collections/AABB.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Graphics/ImageData.hpp"
#include "Graphics/SpriteUVResolver.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/GuiRenderer.hpp"
#include "Math/VectorMath.hpp"
#include "Scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace Index {

	namespace {
		bool BuildEntityAABB(Scene& scene, EntityHandle entity, float worldUIScale, AABB& out) {
			bool has = false;

			Transform2DComponent* tx = nullptr;
			if (scene.TryGetComponent<Transform2DComponent>(entity, tx) && tx) {
				out = AABB::FromTransform(*tx);
				has = true;
			}

			RectTransform2DComponent* rect = nullptr;
			if (scene.TryGetComponent<RectTransform2DComponent>(entity, rect) && rect) {
				const Vec2 bl = rect->GetBottomLeft();
				const Vec2 tr = rect->GetTopRight();
				Vec2 corners[4] = {
					Vec2{ bl.x * worldUIScale, bl.y * worldUIScale },
					Vec2{ tr.x * worldUIScale, bl.y * worldUIScale },
					Vec2{ tr.x * worldUIScale, tr.y * worldUIScale },
					Vec2{ bl.x * worldUIScale, tr.y * worldUIScale },
				};
				if (rect->Rotation != 0.0f) {
					const Vec2 pivot = rect->ResolvedValid ? rect->ResolvedPivot
						: Vec2{ (bl.x + tr.x) * 0.5f, (bl.y + tr.y) * 0.5f };
					const Vec2 worldPivot{ pivot.x * worldUIScale, pivot.y * worldUIScale };
					for (Vec2& c : corners) {
						c = worldPivot + Rotated(c - worldPivot, rect->Rotation);
					}
				}

				Vec2 minP = corners[0];
				Vec2 maxP = corners[0];
				for (int i = 1; i < 4; ++i) {
					minP.x = std::min(minP.x, corners[i].x);
					minP.y = std::min(minP.y, corners[i].y);
					maxP.x = std::max(maxP.x, corners[i].x);
					maxP.y = std::max(maxP.y, corners[i].y);
				}
				AABB rectAABB{ minP, maxP };
				if (!has) {
					out = rectAABB;
					has = true;
				}
				else {
					out.Min.x = std::min(out.Min.x, rectAABB.Min.x);
					out.Min.y = std::min(out.Min.y, rectAABB.Min.y);
					out.Max.x = std::max(out.Max.x, rectAABB.Max.x);
					out.Max.y = std::max(out.Max.y, rectAABB.Max.y);
				}
			}

			return has;
		}

		// ── Sprite alpha mask (pixel-precise picking) ───────────────────────
		// A click on a sprite's transparent texels should fall through to whatever
		// is behind it. We bake the texture slice's opaque/transparent texels into a
		// small bitmap once per (texture asset, sprite slice) — in the renderer's
		// unit-quad baseUV space — and sample it during hit testing. GetImageData()
		// re-decodes from disk, so building is gated behind the cheap oriented-quad
		// test and the result is cached. Mirrors the editor's selection-outline cache.

		struct SpriteMaskKey {
			uint64_t AssetId;
			uint64_t SliceHash;
			bool operator==(const SpriteMaskKey& o) const {
				return AssetId == o.AssetId && SliceHash == o.SliceHash;
			}
		};
		struct SpriteMaskKeyHash {
			size_t operator()(const SpriteMaskKey& k) const {
				return std::hash<uint64_t>{}(k.AssetId) ^ (std::hash<uint64_t>{}(k.SliceHash) << 1);
			}
		};
		struct SpriteMask {
			int W = 0;
			int H = 0;
			std::vector<uint8_t> Opaque; // row-major, baseUV space; 1 = opaque
		};

		// W == 0 entry = "no usable mask" (transparent / undecodable); the caller
		// then falls back to the rectangular quad.
		std::unordered_map<SpriteMaskKey, SpriteMask, SpriteMaskKeyHash> g_SpriteMaskCache;

		SpriteMask BuildSpriteMask(Texture2D& tex, const SpriteRendererComponent& sprite) {
			std::unique_ptr<ImageData> image = tex.GetImageData();
			if (!image || image->Width <= 0 || image->Height <= 0 || !image->Pixels) return {};

			const int w = image->Width;
			const int h = image->Height;
			const unsigned char* px = image->Pixels;

			const SpriteUVRect uv = ResolveSpriteUVRect(sprite.TextureAssetId, sprite.SpriteName, w, h);
			const float uSpan = uv.U1 - uv.U0;
			const float vSpan = uv.V1 - uv.V0;
			if (std::abs(uSpan) < 1e-6f || std::abs(vSpan) < 1e-6f) return {};

			// Mask resolution = slice pixel size, capped so build cost and memory stay bounded.
			constexpr int kCap = 256;
			const int sliceW = std::max(1, static_cast<int>(std::lround(std::abs(uSpan) * w)));
			const int sliceH = std::max(1, static_cast<int>(std::lround(std::abs(vSpan) * h)));
			const int mw = std::min(sliceW, kCap);
			const int mh = std::min(sliceH, kCap);

			constexpr unsigned char kAlphaThreshold = 16;
			auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };

			SpriteMask mask;
			mask.W = mw;
			mask.H = mh;
			mask.Opaque.assign(static_cast<size_t>(mw) * static_cast<size_t>(mh), 0);

			bool any = false;
			for (int my = 0; my < mh; ++my) {
				const float baseY = (static_cast<float>(my) + 0.5f) / static_cast<float>(mh);
				const float v = uv.V0 + baseY * vSpan;
				const int ty = clampi(static_cast<int>(v * static_cast<float>(h)), 0, h - 1);
				const size_t rowBase = static_cast<size_t>(ty) * static_cast<size_t>(w);
				for (int mx = 0; mx < mw; ++mx) {
					const float baseX = (static_cast<float>(mx) + 0.5f) / static_cast<float>(mw);
					const float u = uv.U0 + baseX * uSpan;
					const int tx = clampi(static_cast<int>(u * static_cast<float>(w)), 0, w - 1);
					if (px[(rowBase + static_cast<size_t>(tx)) * 4u + 3u] >= kAlphaThreshold) {
						mask.Opaque[static_cast<size_t>(my) * static_cast<size_t>(mw) + static_cast<size_t>(mx)] = 1;
						any = true;
					}
				}
			}
			if (!any) return {}; // fully transparent slice → fall back to the quad
			return mask;
		}

		// Returns the cached mask, building it on first use. nullptr → no usable mask
		// this call (procedural/loading/transparent texture); caller uses the quad.
		const SpriteMask* GetSpriteMask(const SpriteRendererComponent& sprite) {
			if (static_cast<uint64_t>(sprite.TextureAssetId) == 0) return nullptr;

			const SpriteMaskKey key{
				static_cast<uint64_t>(sprite.TextureAssetId),
				std::hash<std::string>{}(sprite.SpriteName) };

			auto it = g_SpriteMaskCache.find(key);
			if (it == g_SpriteMaskCache.end()) {
				Texture2D* tex = TextureManager::GetTexture(sprite.TextureHandle);
				if (!tex || !tex->IsValid()) return nullptr; // not ready — retry next click, don't cache
				it = g_SpriteMaskCache.emplace(key, BuildSpriteMask(*tex, sprite)).first;
			}
			return it->second.W > 0 ? &it->second : nullptr;
		}

		bool SampleMaskOpaque(const SpriteMask& mask, const Vec2& local) {
			// local in [-0.5, 0.5]^2 → baseUV [0, 1]^2 (inverts the sprite shader's
			// baseUV = (lx + 0.5, 0.5 - ly)).
			const float baseX = local.x + 0.5f;
			const float baseY = 0.5f - local.y;
			int mx = static_cast<int>(baseX * static_cast<float>(mask.W));
			int my = static_cast<int>(baseY * static_cast<float>(mask.H));
			mx = mx < 0 ? 0 : (mx >= mask.W ? mask.W - 1 : mx);
			my = my < 0 ? 0 : (my >= mask.H ? mask.H - 1 : my);
			return mask.Opaque[static_cast<size_t>(my) * static_cast<size_t>(mask.W) + static_cast<size_t>(mx)] != 0;
		}

		// World point → entity's unit-quad local space. Inverts TransformPoint
		// (scale → rotate → translate). false on a degenerate (near-zero) scale.
		bool WorldToQuadLocal(const Transform2DComponent& tx, const Vec2& worldPoint, Vec2& outLocal) {
			if (std::abs(tx.Scale.x) < 1e-8f || std::abs(tx.Scale.y) < 1e-8f) return false;
			const Vec2 rel{ worldPoint.x - tx.Position.x, worldPoint.y - tx.Position.y };
			const Vec2 unrot = Rotated(rel, -tx.Rotation);
			outLocal = Vec2{ unrot.x / tx.Scale.x, unrot.y / tx.Scale.y };
			return true;
		}

		bool HitTestWorldEntity(entt::registry& reg, EntityHandle entity,
			const Vec2& worldPoint, bool pixelPrecise) {
			auto* tx = reg.try_get<Transform2DComponent>(entity);
			if (!tx) return false;

			Vec2 local;
			if (!WorldToQuadLocal(*tx, worldPoint, local)) return false;
			if (std::abs(local.x) > 0.5f || std::abs(local.y) > 0.5f) return false;

			if (pixelPrecise) {
				if (auto* sprite = reg.try_get<SpriteRendererComponent>(entity)) {
					if (const SpriteMask* mask = GetSpriteMask(*sprite)) {
						return SampleMaskOpaque(*mask, local);
					}
				}
			}
			return true; // oriented-quad hit (non-sprite, or sprite without a usable mask)
		}

		bool HitTestUIEntity(entt::registry& reg, EntityHandle entity,
			const Vec2& worldPoint, float worldUIScale) {
			auto* rect = reg.try_get<RectTransform2DComponent>(entity);
			if (!rect) return false;

			const Vec2 bl = rect->GetBottomLeft();
			const Vec2 tr = rect->GetTopRight();

			// Inverse of BuildEntityAABB's forward transform: undo the rotation about
			// the (scaled) pivot, then test against the unrotated scaled rect.
			Vec2 p = worldPoint;
			if (rect->Rotation != 0.0f) {
				const Vec2 pivot = rect->ResolvedValid ? rect->ResolvedPivot
					: Vec2{ (bl.x + tr.x) * 0.5f, (bl.y + tr.y) * 0.5f };
				const Vec2 worldPivot{ pivot.x * worldUIScale, pivot.y * worldUIScale };
				p = worldPivot + Rotated(p - worldPivot, -rect->Rotation);
			}

			const float minX = std::min(bl.x, tr.x) * worldUIScale;
			const float maxX = std::max(bl.x, tr.x) * worldUIScale;
			const float minY = std::min(bl.y, tr.y) * worldUIScale;
			const float maxY = std::max(bl.y, tr.y) * worldUIScale;
			return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY;
		}

		struct PointHit {
			EntityHandle Entity;
			int Group; // 1 = UI, 0 = world; UI always sits on top
			int Layer;
			int Order;
			int Seq;   // iteration order; later wins ties (matches legacy picker)
		};

		// Topmost-first ordering. Reproduces the legacy TryPickEntity winner: UI over
		// world, then highest sorting layer, then order, then latest-iterated.
		bool HitIsAbove(const PointHit& a, const PointHit& b) {
			if (a.Group != b.Group) return a.Group > b.Group;
			if (a.Layer != b.Layer) return a.Layer > b.Layer;
			if (a.Order != b.Order) return a.Order > b.Order;
			return a.Seq > b.Seq;
		}

		void CollectPointHits(Scene& scene, const Vec2& worldPoint, bool pixelPrecise,
			std::vector<PointHit>& outHits) {
			const float worldUIScale = GuiRenderer::ComputeWorldUIPixelScale();
			auto& reg = scene.GetRegistry();
			int seq = 0;

			// World entities: Transform2D without a RectTransform (those are UI, below).
			auto txView = reg.view<Transform2DComponent>();
			for (auto entity : txView) {
				++seq;
				if (reg.try_get<RectTransform2DComponent>(entity)) continue;
				if (!HitTestWorldEntity(reg, entity, worldPoint, pixelPrecise)) continue;

				int layer = 0;
				int order = 0;
				if (auto* sprite = reg.try_get<SpriteRendererComponent>(entity)) {
					layer = static_cast<int>(sprite->SortingLayer);
					order = static_cast<int>(sprite->SortingOrder);
				}
				outHits.push_back(PointHit{ entity, 0, layer, order, seq });
			}

			// UI entities (RectTransform). UI always sits above world-space hits. An
			// entity that (against the Transform2D/RectTransform conflict) carries
			// both is hit by either its UI rect or its world quad, so its sprite
			// region stays clickable while it still counts as a single UI hit.
			auto rectView = reg.view<RectTransform2DComponent>();
			for (auto entity : rectView) {
				++seq;
				bool hit = HitTestUIEntity(reg, entity, worldPoint, worldUIScale);
				if (!hit && reg.try_get<Transform2DComponent>(entity)) {
					hit = HitTestWorldEntity(reg, entity, worldPoint, pixelPrecise);
				}
				if (!hit) continue;
				outHits.push_back(PointHit{ entity, 1, 0, 0, seq });
			}
		}
	}

	bool EntityPicker::TryPickEntity(Scene& scene, const Vec2& worldPoint, EntityHandle& outEntity,
		bool pixelPrecise) {
		outEntity = entt::null;

		std::vector<PointHit> hits;
		CollectPointHits(scene, worldPoint, pixelPrecise, hits);
		if (hits.empty()) return false;

		const PointHit* best = &hits[0];
		for (size_t i = 1; i < hits.size(); ++i) {
			if (HitIsAbove(hits[i], *best)) best = &hits[i];
		}
		outEntity = best->Entity;
		return true;
	}

	void EntityPicker::PickEntitiesAtPoint(Scene& scene, const Vec2& worldPoint,
		std::vector<EntityHandle>& outEntities, bool pixelPrecise) {
		outEntities.clear();

		std::vector<PointHit> hits;
		CollectPointHits(scene, worldPoint, pixelPrecise, hits);
		std::stable_sort(hits.begin(), hits.end(), HitIsAbove);

		outEntities.reserve(hits.size());
		for (const PointHit& h : hits) outEntities.push_back(h.Entity);
	}

	void EntityPicker::PickEntitiesInRect(Scene& scene, const AABB& worldRect, std::vector<EntityHandle>& outEntities) {
		outEntities.clear();

		const float worldUIScale = GuiRenderer::ComputeWorldUIPixelScale();
		auto& reg = scene.GetRegistry();

		auto txView = reg.view<Transform2DComponent>();
		for (auto entity : txView) {
			AABB aabb;
			if (!BuildEntityAABB(scene, entity, worldUIScale, aabb)) continue;
			if (AABB::Intersects(aabb, worldRect)) outEntities.push_back(entity);
		}

		// UI-only entities (RectTransform without Transform2D) aren't in txView.
		auto rectView = reg.view<RectTransform2DComponent>();
		for (auto entity : rectView) {
			if (reg.try_get<Transform2DComponent>(entity)) continue;
			AABB aabb;
			if (!BuildEntityAABB(scene, entity, worldUIScale, aabb)) continue;
			if (AABB::Intersects(aabb, worldRect)) outEntities.push_back(entity);
		}
	}

}
