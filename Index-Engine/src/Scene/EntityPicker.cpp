#include "pch.hpp"
#include "Scene/EntityPicker.hpp"

#include "Collections/AABB.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Gui/GuiRenderer.hpp"
#include "Math/VectorMath.hpp"
#include "Scene/Scene.hpp"

#include <algorithm>
#include <limits>

namespace Index {

	namespace {
		// Combined Transform2D + RectTransform2D AABB for a single entity.
		// Mirrors the merge logic in ImGuiEditorLayer::TryBuildEntityAABB but
		// scoped to one entity (no child traversal) and skips the
		// Propagate / ComputeUILayout calls — those are the caller's job.
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
	}

	bool EntityPicker::TryPickEntity(Scene& scene, const Vec2& worldPoint, EntityHandle& outEntity) {
		outEntity = entt::null;

		const float worldUIScale = GuiRenderer::ComputeWorldUIPixelScale();
		auto& reg = scene.GetRegistry();

		// Pass 1: world-space entities (Transform2DComponent). Sort hits by
		// (SortingLayer, SortingOrder) descending so the front-most sprite
		// wins overlapping picks. Non-sprite entities sort at (0, 0); ties
		// fall to entt iteration order (later iterated = on top).
		EntityHandle picked = entt::null;
		int bestLayer = std::numeric_limits<int>::min();
		int bestOrder = std::numeric_limits<int>::min();
		auto txView = reg.view<Transform2DComponent>();
		for (auto entity : txView) {
			AABB aabb;
			if (!BuildEntityAABB(scene, entity, worldUIScale, aabb)) continue;
			if (!AABB::Contains(aabb, worldPoint)) continue;

			int layer = 0;
			int order = 0;
			if (auto* sprite = reg.try_get<SpriteRendererComponent>(entity)) {
				layer = static_cast<int>(sprite->SortingLayer);
				order = static_cast<int>(sprite->SortingOrder);
			}
			if (picked == entt::null
				|| layer > bestLayer
				|| (layer == bestLayer && order >= bestOrder))
			{
				picked = entity;
				bestLayer = layer;
				bestOrder = order;
			}
		}

		// Pass 2: UI entities (RectTransform2DComponent, mutually exclusive
		// with Transform2D — see DeclareConflict in BuiltInComponentRegistration).
		// UI draws on top of world sprites in the editor's uiInWorldSpace
		// render, so any UI hit overrides a world-space hit; among UI hits,
		// later iterated wins.
		auto rectView = reg.view<RectTransform2DComponent>();
		for (auto entity : rectView) {
			AABB aabb;
			if (!BuildEntityAABB(scene, entity, worldUIScale, aabb)) continue;
			if (!AABB::Contains(aabb, worldPoint)) continue;
			picked = entity;
		}

		if (picked == entt::null) {
			return false;
		}
		outEntity = picked;
		return true;
	}

}
