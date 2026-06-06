#include <pch.hpp>
#include "Systems/ImGuiEditorLayer.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <glm/gtc/matrix_transform.hpp>

#include "Components/Forward.hpp"
#include "Components/General/General.hpp"
#include "Components/UI/UI.hpp"
#include "Components/Tags.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Physics/BoxCollider2DComponent.hpp"
#include "Components/Physics/CircleCollider2DComponent.hpp"
#include "Components/Physics/PolygonCollider2DComponent.hpp"
#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Components/Physics/FastBody2DComponent.hpp"
#include "Components/Physics/FastBoxCollider2DComponent.hpp"
#include "Components/Physics/FastCircleCollider2DComponent.hpp"
#include "Collections/AspectRatio.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Diagnostics/StatsOverlay.hpp"
#include "Editor/ApplicationEditorAccess.hpp"
#include "Graphics/Framebuffer.hpp"
#include "Graphics/GizmoRenderer.hpp"
#include "Graphics/Gizmo.hpp"
#include "Graphics/RenderApi.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Graphics/SpriteUVResolver.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/GuiRenderer.hpp"
#include "Gui/EditorIcons.hpp"
#include "Math/Trigonometry.hpp"
#include "Math/VectorMath.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/ComponentInfo.hpp"
#include "Editor/EditorPreferences.hpp"
#include "Scene/EntityPicker.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Systems/TransformHierarchySystem.hpp"
#include "Systems/UILayoutSystem.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Index {

	namespace {
		// ── Sprite selection outline ────────────────────────────────────────
		// The editor selection gizmo traces a sprite's opaque silhouette rather
		// than its rectangular quad. We compute the convex hull of the texture's
		// non-transparent texels once per (texture asset, sprite slice), in the
		// renderer's unit-quad local space ([-0.5, 0.5]²), and cache it. Each
		// frame the gizmo transforms those points by the entity transform, so the
		// outline hugs the texture and follows rotation/scale. Full-bleed textures
		// collapse to the four quad corners, matching the old behaviour.

		struct SpriteOutlineKey {
			uint64_t AssetId;
			uint64_t SliceHash;
			bool operator==(const SpriteOutlineKey& o) const {
				return AssetId == o.AssetId && SliceHash == o.SliceHash;
			}
		};
		struct SpriteOutlineKeyHash {
			size_t operator()(const SpriteOutlineKey& k) const {
				return std::hash<uint64_t>{}(k.AssetId) ^ (std::hash<uint64_t>{}(k.SliceHash) << 1);
			}
		};

		// Empty vector = "no silhouette" (fully transparent / undecodable); the
		// caller then falls back to the rectangular quad.
		std::unordered_map<SpriteOutlineKey, std::vector<Vec2>, SpriteOutlineKeyHash> g_SpriteOutlineCache;

		std::vector<Vec2> BuildSpriteOutline(Texture2D& tex, const SpriteRendererComponent& sprite) {
			std::unique_ptr<ImageData> image = tex.GetImageData();
			if (!image || image->Width <= 0 || image->Height <= 0 || !image->Pixels) return {};

			const int w = image->Width;
			const int h = image->Height;
			const unsigned char* px = image->Pixels;

			const SpriteUVRect uv = ResolveSpriteUVRect(sprite.TextureAssetId, sprite.SpriteName, w, h);
			const float uSpan = uv.U1 - uv.U0;
			const float vSpan = uv.V1 - uv.V0;
			if (std::abs(uSpan) < 1e-6f || std::abs(vSpan) < 1e-6f) return {};

			auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
			const int sx0 = clampi(static_cast<int>(std::floor(std::min(uv.U0, uv.U1) * w)), 0, w);
			const int sx1 = clampi(static_cast<int>(std::ceil (std::max(uv.U0, uv.U1) * w)), 0, w);
			const int sy0 = clampi(static_cast<int>(std::floor(std::min(uv.V0, uv.V1) * h)), 0, h);
			const int sy1 = clampi(static_cast<int>(std::ceil (std::max(uv.V0, uv.V1) * h)), 0, h);
			if (sx1 <= sx0 || sy1 <= sy0) return {};

			// Stride-sample large textures so selecting never hitches.
			const int stride = std::max(1, std::max(sx1 - sx0, sy1 - sy0) / 512);
			constexpr unsigned char kAlphaThreshold = 16;

			// Per-row L/R and per-column T/B opaque extremes: their convex hull
			// equals the hull of every opaque texel, at only O(W + H) points.
			std::vector<int> rowMinX(h, -1), rowMaxX(h, -1);
			std::vector<int> colMinY(w, -1), colMaxY(w, -1);
			bool any = false;
			for (int y = sy0; y < sy1; y += stride) {
				const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(w);
				for (int x = sx0; x < sx1; x += stride) {
					if (px[(rowBase + static_cast<size_t>(x)) * 4u + 3u] < kAlphaThreshold) continue;
					any = true;
					if (rowMinX[y] < 0 || x < rowMinX[y]) rowMinX[y] = x;
					if (x > rowMaxX[y]) rowMaxX[y] = x;
					if (colMinY[x] < 0 || y < colMinY[x]) colMinY[x] = y;
					if (y > colMaxY[x]) colMaxY[x] = y;
				}
			}
			if (!any) return {};

			struct P { long long x, y; };
			std::vector<P> pts;
			for (int y = sy0; y < sy1; ++y) {
				if (rowMinX[y] < 0) continue;
				pts.push_back({ rowMinX[y], y });
				pts.push_back({ rowMaxX[y], y });
			}
			for (int x = sx0; x < sx1; ++x) {
				if (colMinY[x] < 0) continue;
				pts.push_back({ x, colMinY[x] });
				pts.push_back({ x, colMaxY[x] });
			}

			std::sort(pts.begin(), pts.end(),
				[](const P& a, const P& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
			pts.erase(std::unique(pts.begin(), pts.end(),
				[](const P& a, const P& b) { return a.x == b.x && a.y == b.y; }), pts.end());

			// Andrew's monotone-chain convex hull (integer math, exact).
			const int n = static_cast<int>(pts.size());
			std::vector<P> hull;
			if (n < 3) {
				hull = pts;
			}
			else {
				auto cross = [](const P& o, const P& a, const P& b) -> long long {
					return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
				};
				hull.resize(static_cast<size_t>(2 * n));
				int k = 0;
				for (int i = 0; i < n; ++i) {
					while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) k--;
					hull[k++] = pts[i];
				}
				for (int i = n - 2, t = k + 1; i >= 0; --i) {
					while (k >= t && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) k--;
					hull[k++] = pts[i];
				}
				hull.resize(static_cast<size_t>(k - 1));
			}
			if (hull.size() < 3) return {};

			// Invert the sprite shader: baseUV = (lx + 0.5, 0.5 - ly);
			// uv = mix(UvRect.xy, UvRect.zw, baseUV). Texel centres → local space.
			std::vector<Vec2> local;
			local.reserve(hull.size());
			for (const P& p : hull) {
				const float u = (static_cast<float>(p.x) + 0.5f) / static_cast<float>(w);
				const float v = (static_cast<float>(p.y) + 0.5f) / static_cast<float>(h);
				const float baseX = (u - uv.U0) / uSpan;
				const float baseY = (v - uv.V0) / vSpan;
				local.push_back(Vec2{ baseX - 0.5f, 0.5f - baseY });
			}
			return local;
		}

		// Returns the cached outline, building it on first use. nullptr means
		// "no silhouette this frame" (texture not resolved yet, or transparent) —
		// the caller draws the rectangular quad instead.
		const std::vector<Vec2>* GetSpriteSelectionOutline(const SpriteRendererComponent& sprite) {
			if (static_cast<uint64_t>(sprite.TextureAssetId) == 0) return nullptr;

			const SpriteOutlineKey key{
				static_cast<uint64_t>(sprite.TextureAssetId),
				std::hash<std::string>{}(sprite.SpriteName) };

			auto it = g_SpriteOutlineCache.find(key);
			if (it == g_SpriteOutlineCache.end()) {
				Texture2D* tex = TextureManager::GetTexture(sprite.TextureHandle);
				if (!tex || !tex->IsValid()) return nullptr; // not ready — retry next frame, don't cache
				it = g_SpriteOutlineCache.emplace(key, BuildSpriteOutline(*tex, sprite)).first;
			}
			return it->second.size() >= 3 ? &it->second : nullptr;
		}
	}

	void ImGuiEditorLayer::RenderSceneIntoFBO(Framebuffer& fbo, Scene& scene,
		const glm::mat4& vp, const AABB& viewportAABB,
		bool withGizmos, bool sharedGizmosOnly, const Color& clearColor,
		bool onlyPassedScene, bool uiInWorldSpace,
		EditorViewDrawMode drawMode,
		GizmoLayerMask gizmoLayerMask)
	{
		auto* app = Application::GetInstance();
		if (!app) return;
		auto* renderer = app->GetRenderer2D();
		if (!renderer) return;

		struct RenderStateGuard {
			PolygonMode PreviousPolygonMode = PolygonMode::Filled;

			~RenderStateGuard() {
				RenderApi::SetPolygonMode(PreviousPolygonMode);
				RenderApi::SetColorMask(true, true, true, true);
			}
		};

		RenderStateGuard stateGuard{
			RenderApi::GetPolygonMode()
		};
		RenderApi::SetPolygonMode(PolygonMode::Filled);
		RenderApi::SetColorMask(true, true, true, true);

		const int w = fbo.GetWidth();
		const int h = fbo.GetHeight();

		// All immediate-mode state goes through RenderApi — no glXxx calls
		// in editor code. Backend translation lives in
		// `Graphics/Backend/WebGPUApi.cpp`.
		RenderApi::BindFramebuffer(fbo);
		RenderApi::SetViewport(0, 0, w, h);
		RenderApi::SetClearColor(clearColor);
		RenderApi::Clear(ClearFlags::Color | ClearFlags::Depth);

		// Sprites-only lambda: only the sprite pipeline honours PolygonMode::Wireframe; GuiRenderer/GizmoRenderer2D paint filled quads regardless and would mask it out.
		auto runSpriteRender = [&]() {
			if (onlyPassedScene) {
				renderer->RenderSceneWithVP(scene, vp, viewportAABB);
			}
			else {
				SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
					renderer->RenderSceneWithVP(s, vp, viewportAABB);
					});
			}
		};

		auto runOverlayRender = [&]() {
			// UI submits BEFORE gizmos so selection outlines and manipulators paint on top even when UI fills the viewport.
			if (auto* gui = app->GetGuiRenderer()) {
				const float pixelToWorldScale = uiInWorldSpace
					? GuiRenderer::ComputeWorldUIPixelScale()
					: 0.0f;
				auto renderOne = [&](Scene& s) {
					if (uiInWorldSpace) {
						gui->RenderScene(s, vp, pixelToWorldScale);
					} else {
						gui->RenderScene(s);
					}
				};
				if (onlyPassedScene) {
					renderOne(scene);
				}
				else {
					SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
						renderOne(s);
						});
				}
			}

			if (withGizmos && Gizmo::IsEnabled()) {
				const GizmoLayerMask layerMask = sharedGizmosOnly ? GizmoLayerMask::Shared : gizmoLayerMask;
				GizmoRenderer2D::RenderWithVP(vp, layerMask);
			}
		};

		auto runWireframePass = [&]() {
			RenderApi::SetPolygonMode(PolygonMode::Wireframe);
			RenderApi::SetColorMask(true, true, true, false);
			// Sprites only — UI and gizmos don't honour the wireframe flag
			// and would paint filled quads on top of the wireframe edges.
			runSpriteRender();
			RenderApi::SetColorMask(true, true, true, true);
			RenderApi::SetPolygonMode(PolygonMode::Filled);
		};

		auto runFilledPass = [&]() {
			RenderApi::SetPolygonMode(PolygonMode::Filled);
			RenderApi::SetColorMask(true, true, true, true);
			runSpriteRender();
			runOverlayRender();
		};

		switch (drawMode) {
		case EditorViewDrawMode::Triangle:
			runWireframePass();
			runOverlayRender();
			break;
		case EditorViewDrawMode::Mixed:
			runFilledPass();
			runWireframePass();
			break;
		case EditorViewDrawMode::Default:
		default:
			runFilledPass();
			break;
		}

		RenderApi::BindDefaultFramebuffer();

		auto* window = Application::GetWindow();
		if (window) {
			RenderApi::SetViewport(0, 0, window->GetWidth(), window->GetHeight());
		}
	}

	void ImGuiEditorLayer::DrawSnapGrid(const AABB& viewBounds) {
		const float gx = EditorPreferences::GetGridSizeX();
		const float gy = EditorPreferences::GetGridSizeY();
		if (gx <= 0.0f || gy <= 0.0f) return;

		// Skip when the grid would be too dense to read (and a draw-call hazard).
		constexpr float k_MaxLinesPerAxis = 220.0f;
		if ((viewBounds.Max.x - viewBounds.Min.x) / gx > k_MaxLinesPerAxis) return;
		if ((viewBounds.Max.y - viewBounds.Min.y) / gy > k_MaxLinesPerAxis) return;

		const Color previousColor = Gizmo::GetColor();
		const float previousLineWidth = Gizmo::GetLineWidth();
		const GizmoLayer previousLayer = Gizmo::GetLayer();

		Gizmo::SetLayer(GizmoLayer::EditorOnly);
		Gizmo::SetLineWidth(1.0f);

		Gizmo::SetColor(Color(0.55f, 0.55f, 0.62f, 0.16f));

		// Cell boundaries sit half a cell off the snap points (which are multiples
		// of the grid size), so an entity snapped to a multiple lands centered in a
		// cell rather than straddling a line crossing.
		const float startX = (std::ceil(viewBounds.Min.x / gx - 0.5f) + 0.5f) * gx;
		for (float x = startX; x <= viewBounds.Max.x; x += gx) {
			Gizmo::DrawLine(Vec2{ x, viewBounds.Min.y }, Vec2{ x, viewBounds.Max.y });
		}
		const float startY = (std::ceil(viewBounds.Min.y / gy - 0.5f) + 0.5f) * gy;
		for (float y = startY; y <= viewBounds.Max.y; y += gy) {
			Gizmo::DrawLine(Vec2{ viewBounds.Min.x, y }, Vec2{ viewBounds.Max.x, y });
		}

		Gizmo::SetColor(previousColor);
		Gizmo::SetLineWidth(previousLineWidth);
		Gizmo::SetLayer(previousLayer);
	}

	void ImGuiEditorLayer::DrawWorldAxes(const AABB& viewBounds) {
		const Color previousColor = Gizmo::GetColor();
		const float previousLineWidth = Gizmo::GetLineWidth();
		const GizmoLayer previousLayer = Gizmo::GetLayer();

		Gizmo::SetLayer(GizmoLayer::EditorOnly);
		Gizmo::SetLineWidth(1.5f);

		// X axis (horizontal, y = 0) in red; only emitted when the origin row is on-screen.
		if (viewBounds.Min.y <= 0.0f && viewBounds.Max.y >= 0.0f) {
			Gizmo::SetColor(Color(0.85f, 0.25f, 0.25f, 0.85f));
			Gizmo::DrawLine(Vec2{ viewBounds.Min.x, 0.0f }, Vec2{ viewBounds.Max.x, 0.0f });
		}
		// Y axis (vertical, x = 0) in green; only emitted when the origin column is on-screen.
		if (viewBounds.Min.x <= 0.0f && viewBounds.Max.x >= 0.0f) {
			Gizmo::SetColor(Color(0.30f, 0.80f, 0.35f, 0.85f));
			Gizmo::DrawLine(Vec2{ 0.0f, viewBounds.Min.y }, Vec2{ 0.0f, viewBounds.Max.y });
		}

		Gizmo::SetColor(previousColor);
		Gizmo::SetLineWidth(previousLineWidth);
		Gizmo::SetLayer(previousLayer);
	}

	void ImGuiEditorLayer::DrawEditorComponentGizmos(Scene& scene, bool componentGizmosEnabled) {
		const Color previousColor = Gizmo::GetColor();
		const float previousLineWidth = Gizmo::GetLineWidth();
		const GizmoLayer previousLayer = Gizmo::GetLayer();
		Gizmo::SetLayer(GizmoLayer::EditorOnly);

		// Always-visible component gizmos (Camera frames, package components flagged
		// drawEditorGizmoAlways): drawn for every matching entity, gated only on the
		// editor-view gizmo toggle — independent of selection.
		if (componentGizmosEnabled) {
			DrawAlwaysVisibleComponentGizmos(scene);
		}

		// Selection-only gizmos: draw each selected entity's gizmos by reusing the
		// single-entity path; m_SelectedEntity is that path's cursor, so save/restore
		// it around the walk.
		const std::vector<EntityHandle> selection = GetSelectedEntities(scene);

		// RectTransform gizmos read resolved corners, so refresh the UI layout once
		// here (it's a whole-scene pass) rather than once per selected entity.
		for (EntityHandle entity : selection) {
			if (entity != entt::null && scene.IsValid(entity)
				&& scene.HasComponent<RectTransform2DComponent>(entity)) {
				ComputeUILayout(scene);
				break;
			}
		}

		if (selection.size() <= 1) {
			DrawComponentGizmosForActiveEntity(scene, componentGizmosEnabled);
		}
		else {
			const EntityHandle activeEntity = m_SelectedEntity;
			for (EntityHandle entity : selection) {
				m_SelectedEntity = entity;
				DrawComponentGizmosForActiveEntity(scene, componentGizmosEnabled);
			}
			m_SelectedEntity = activeEntity;
		}

		Gizmo::SetLayer(previousLayer);
		Gizmo::SetColor(previousColor);
		Gizmo::SetLineWidth(previousLineWidth);
	}

	void ImGuiEditorLayer::DrawAlwaysVisibleComponentGizmos(Scene& scene) {
		// Built-in: Camera viewport frame for every camera in the scene.
		auto camView = scene.GetRegistry().view<Camera2DComponent, Transform2DComponent>();
		for (auto entity : camView) {
			DrawCameraFrameGizmo(scene, entity);
		}

		// Package/registry components opted into always-draw via SetEditorGizmo(fn, true):
		// run their gizmo for every entity that has the component.
		auto* app = Application::GetInstance();
		auto* sm = app ? app->GetSceneManager() : nullptr;
		if (!sm) return;
		auto allEntities = scene.GetRegistry().view<entt::entity>();
		sm->GetComponentRegistry().ForEachComponentInfo(
			[&](const std::type_index&, const ComponentInfo& info) {
				if (!info.drawEditorGizmo || !info.drawEditorGizmoAlways || !info.has) return;
				for (auto handle : allEntities) {
					Entity entity = scene.GetEntity(handle);
					if (info.has(entity)) info.drawEditorGizmo(entity);
				}
			});
	}

	void ImGuiEditorLayer::DrawCameraFrameGizmo(Scene& scene, EntityHandle entity) {
		Transform2DComponent* transform = nullptr;
		Camera2DComponent* camera = nullptr;
		if (!scene.TryGetComponent<Transform2DComponent>(entity, transform) || !transform) return;
		if (!scene.TryGetComponent<Camera2DComponent>(entity, camera) || !camera) return;

		Gizmo::SetColor(Color::White());
		Gizmo::SetLineWidth(1.5f);
		// Frame at the configured Game View aspect, not the live window aspect — WorldViewPort() tracks m_Viewport (the OS window) here, so it overstates the camera's horizontal view. Free Aspect (0) has no fixed ratio → keep the window aspect.
		Vec2 camFrame = camera->WorldViewPort();
		const int gvAspectIdx = std::clamp(m_GameViewAspectPresetIndex, 0, static_cast<int>(k_AspectRatioPresets.size()) - 1);
		const float gvAspect = k_AspectRatioPresets[gvAspectIdx].Aspect;
		if (gvAspect > 0.0f) camFrame.x = camFrame.y * gvAspect;
		Gizmo::DrawSquare(transform->Position, camFrame, transform->GetRotationDegrees());
	}

	void ImGuiEditorLayer::DrawComponentGizmosForActiveEntity(Scene& scene, bool componentGizmosEnabled) {
		if (m_SelectedEntity == entt::null || !scene.IsValid(m_SelectedEntity)) {
			return;
		}

		const bool hasTransform = scene.HasComponent<Transform2DComponent>(m_SelectedEntity);
		const bool hasRectTransform = scene.HasComponent<RectTransform2DComponent>(m_SelectedEntity);

		// Package gizmo callbacks may target transformless entities, so no early-out on missing transform.

		const Color previousColor = Gizmo::GetColor();
		const float previousLineWidth = Gizmo::GetLineWidth();
		const GizmoLayer previousLayer = Gizmo::GetLayer();

		Gizmo::SetLayer(GizmoLayer::EditorOnly);

		if (componentGizmosEnabled && hasTransform) {
			auto& transform = scene.GetComponent<Transform2DComponent>(m_SelectedEntity);
			const float rotationDegrees = transform.GetRotationDegrees();
	
			if (scene.HasComponent<SpriteRendererComponent>(m_SelectedEntity)) {
				const auto& sprite = scene.GetComponent<SpriteRendererComponent>(m_SelectedEntity);
				Gizmo::SetColor(Color(1.0f, 0.65f, 0.10f, 1.0f));
				Gizmo::SetLineWidth(2.0f);

				// Outline the texture's opaque silhouette (cached, in unit-quad
				// local space); TransformPoint folds in Scale/Rotation/Position so
				// it tracks the rendered sprite. Fall back to the quad when the
				// texture has no silhouette (procedural, transparent, or loading).
				if (const std::vector<Vec2>* outline = GetSpriteSelectionOutline(sprite)) {
					const std::vector<Vec2>& pts = *outline;
					for (size_t i = 0; i < pts.size(); ++i) {
						const Vec2 a = transform.TransformPoint(pts[i]);
						const Vec2 b = transform.TransformPoint(pts[(i + 1) % pts.size()]);
						Gizmo::DrawLine(a, b);
					}
				}
				else {
					Gizmo::DrawSquare(transform.Position, transform.Scale, rotationDegrees);
				}
			}

			// Camera frame is drawn for ALL cameras in DrawAlwaysVisibleComponentGizmos,
			// not only the selected one.
	
			if (scene.HasComponent<BoxCollider2DComponent>(m_SelectedEntity)) {
				auto& collider = scene.GetComponent<BoxCollider2DComponent>(m_SelectedEntity);
				if (collider.IsValid()) {
					const Vec2 center = transform.Position + Rotated(collider.GetCenter(), transform.Rotation);
					Gizmo::SetColor(Color(0.20f, 1.0f, 0.35f, 1.0f));
					Gizmo::SetLineWidth(2.0f);
					Gizmo::DrawSquare(center, collider.GetScale(), rotationDegrees);
				}
			}
	
			if (scene.HasComponent<CircleCollider2DComponent>(m_SelectedEntity)) {
				auto& collider = scene.GetComponent<CircleCollider2DComponent>(m_SelectedEntity);
				if (collider.IsValid()) {
					const Vec2 center = transform.Position + Rotated(collider.GetCenter(), transform.Rotation);
					Gizmo::SetColor(Color(0.20f, 1.0f, 0.35f, 1.0f));
					Gizmo::SetLineWidth(2.0f);
					Gizmo::DrawCircle(center, collider.GetRadius());
				}
			}
	
			if (scene.HasComponent<PolygonCollider2DComponent>(m_SelectedEntity)) {
				auto& collider = scene.GetComponent<PolygonCollider2DComponent>(m_SelectedEntity);
				if (collider.IsValid()) {
					const std::vector<Vec2> worldPoints = collider.GetWorldPoints();
					if (worldPoints.size() >= 3) {
						Gizmo::SetColor(Color(0.20f, 1.0f, 0.35f, 1.0f));
						Gizmo::SetLineWidth(2.0f);
						const float rot = transform.Rotation;
						for (size_t i = 0; i < worldPoints.size(); ++i) {
							const Vec2 a = transform.Position + Rotated(worldPoints[i], rot);
							const Vec2 b = transform.Position + Rotated(worldPoints[(i + 1) % worldPoints.size()], rot);
							Gizmo::DrawLine(a, b);
						}
					}
				}
			}
	
			if (scene.HasComponent<FastBoxCollider2DComponent>(m_SelectedEntity)) {
				auto& collider = scene.GetComponent<FastBoxCollider2DComponent>(m_SelectedEntity);
				// GetHalfExtents() returns the live IndexPhys collider's
				// world-scaled extents (SyncWithTransform already folded
				// Transform2D.Scale in), so it must NOT be multiplied by scale
				// again — that double-scaled the gizmo. Only the no-live-
				// collider fallback returns the raw authored field and needs
				// the manual scale multiply.
				const Vec2 he = collider.IsValid()
					? collider.GetHalfExtents()
					: Vec2{ collider.HalfExtents.x * transform.Scale.x,
					        collider.HalfExtents.y * transform.Scale.y };
				const Vec2 worldSize(std::abs(he.x) * 2.0f, std::abs(he.y) * 2.0f);
				Gizmo::SetColor(Color(0.10f, 0.85f, 0.85f, 1.0f));
				Gizmo::SetLineWidth(2.0f);
				Gizmo::DrawSquare(transform.Position, worldSize, rotationDegrees);
			}

			if (scene.HasComponent<FastCircleCollider2DComponent>(m_SelectedEntity)) {
				auto& collider = scene.GetComponent<FastCircleCollider2DComponent>(m_SelectedEntity);
				// Same double-scale fix as the box above: GetRadius() is the
				// live scaled radius; only the fallback re-applies scale.
				const float worldRadius = collider.IsValid()
					? collider.GetRadius()
					: collider.Radius * std::max(std::abs(transform.Scale.x), std::abs(transform.Scale.y));
				Gizmo::SetColor(Color(0.10f, 0.85f, 0.85f, 1.0f));
				Gizmo::SetLineWidth(2.0f);
				Gizmo::DrawCircle(transform.Position, worldRadius);
			}
	
			if (scene.HasComponent<ParticleSystem2DComponent>(m_SelectedEntity)) {
				auto& particleSystem = scene.GetComponent<ParticleSystem2DComponent>(m_SelectedEntity);
				Gizmo::SetColor(Color(1.0f, 0.20f, 0.75f, 1.0f));
				Gizmo::SetLineWidth(2.0f);
	
				std::visit([&](auto&& shape) {
					using T = std::decay_t<decltype(shape)>;
					if constexpr (std::is_same_v<T, ParticleSystem2DComponent::CircleParams>) {
						const float radius = shape.Radius * std::max(std::abs(transform.Scale.x), std::abs(transform.Scale.y));
						const float arc = std::clamp(shape.Arc, 0.0f, 360.0f);
						if (arc >= 359.999f) {
							Gizmo::DrawCircle(transform.Position, radius);
						}
						else {
							// Sector matching the emission range: sweep `arc` degrees from the
							// entity's local +X (rotated by its rotation), plus the bounding radii.
							const Vec2 center = transform.Position;
							const float base = transform.Rotation;
							const float sweep = arc * 0.01745329252f;
							const int segments = std::max(2, static_cast<int>(arc / 6.0f) + 1);
							Vec2 prev = center + FromAngle(base) * radius;
							Gizmo::DrawLine(center, prev);
							for (int seg = 1; seg <= segments; ++seg) {
								const float a = base + sweep * (static_cast<float>(seg) / static_cast<float>(segments));
								const Vec2 cur = center + FromAngle(a) * radius;
								Gizmo::DrawLine(prev, cur);
								prev = cur;
							}
							Gizmo::DrawLine(center, prev);
						}
					}
					else if constexpr (std::is_same_v<T, ParticleSystem2DComponent::SquareParams>) {
						const Vec2 size(
							std::abs(shape.HalfExtends.x * transform.Scale.x) * 2.0f,
							std::abs(shape.HalfExtends.y * transform.Scale.y) * 2.0f);
						Gizmo::DrawSquare(transform.Position, size, rotationDegrees);
					}
					else if constexpr (std::is_same_v<T, ParticleSystem2DComponent::EdgeParams>) {
						const float halfLength = shape.Length * 0.5f;
						Gizmo::DrawLine(
							transform.TransformPoint(Vec2{ -halfLength, 0.0f }),
							transform.TransformPoint(Vec2{ halfLength, 0.0f }));
					}
				}, particleSystem.Shape);
	
				Vec2 moveDirection = particleSystem.ParticleSettings.MoveDirection;
				if (LengthSquared(moveDirection) < 0.0001f) {
					moveDirection = Up();
				}
				moveDirection = Normalized(Rotated(moveDirection, transform.Rotation));
				const float indicatorLength = std::max(0.75f, std::max(std::abs(transform.Scale.x), std::abs(transform.Scale.y)));
				Gizmo::DrawLine(transform.Position, transform.Position + moveDirection * indicatorLength);
			}
		} // end if (hasTransform)

		if (hasRectTransform) {
			// Layout is refreshed once up front by DrawEditorComponentGizmos (before
			// the selection walk), so the resolved corners read below are current.
			auto& rect = scene.GetComponent<RectTransform2DComponent>(m_SelectedEntity);

			// Mirror RenderSceneIntoFBO's uiInWorldSpace=true scale so the outline lands on the rendered widget.
			const float worldScale = GuiRenderer::ComputeWorldUIPixelScale();

			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const Vec2 pivot = rect.ResolvedValid ? rect.ResolvedPivot
				: Vec2{ (bl.x + tr.x) * 0.5f, (bl.y + tr.y) * 0.5f };

			Vec2 corners[4] = {
				Vec2{ bl.x * worldScale, bl.y * worldScale },
				Vec2{ tr.x * worldScale, bl.y * worldScale },
				Vec2{ tr.x * worldScale, tr.y * worldScale },
				Vec2{ bl.x * worldScale, tr.y * worldScale },
			};

			// Rotate around the resolved pivot (in world units now) so
			// non-centered pivots match what the renderer draws.
			if (rect.Rotation != 0.0f) {
				const Vec2 worldPivot{ pivot.x * worldScale, pivot.y * worldScale };
				for (int i = 0; i < 4; ++i) {
					corners[i] = worldPivot + Rotated(corners[i] - worldPivot, rect.Rotation);
				}
			}

			Gizmo::SetColor(Color(0.30f, 0.80f, 1.0f, 1.0f));
			Gizmo::SetLineWidth(2.0f);
			Gizmo::DrawLine(corners[0], corners[1]);
			Gizmo::DrawLine(corners[1], corners[2]);
			Gizmo::DrawLine(corners[2], corners[3]);
			Gizmo::DrawLine(corners[3], corners[0]);

			const AABB rectHandleCamAABB = m_EditorCamera.GetViewportAABB();
			const float rectHandleWorldPerPx = rectHandleCamAABB.Scale().x / std::max(1.0f, static_cast<float>(m_EditorViewFBO.GetWidth()));
			const float rectHandleHalf = 5.0f * rectHandleWorldPerPx;
			const Vec2 rectHandleSize{ rectHandleHalf * 2.0f, rectHandleHalf * 2.0f };
			const float rectHandleRotationDegrees = Degrees(rect.Rotation);
			const Vec2 outerMidL{ (corners[0].x + corners[3].x) * 0.5f, (corners[0].y + corners[3].y) * 0.5f };
			const Vec2 outerMidR{ (corners[1].x + corners[2].x) * 0.5f, (corners[1].y + corners[2].y) * 0.5f };
			const Vec2 outerMidB{ (corners[0].x + corners[1].x) * 0.5f, (corners[0].y + corners[1].y) * 0.5f };
			const Vec2 outerMidT{ (corners[2].x + corners[3].x) * 0.5f, (corners[2].y + corners[3].y) * 0.5f };
			Gizmo::DrawSquare(outerMidL, rectHandleSize, rectHandleRotationDegrees);
			Gizmo::DrawSquare(outerMidR, rectHandleSize, rectHandleRotationDegrees);
			Gizmo::DrawSquare(outerMidB, rectHandleSize, rectHandleRotationDegrees);
			Gizmo::DrawSquare(outerMidT, rectHandleSize, rectHandleRotationDegrees);

			if (componentGizmosEnabled && scene.HasComponent<TextRendererComponent>(m_SelectedEntity)) {
				const auto& text = scene.GetComponent<TextRendererComponent>(m_SelectedEntity);

				const float marginScale = worldScale * std::max(0.01f, std::abs(rect.Scale.x));
				const float ml = text.Margin.x * marginScale;
				const float mt = text.Margin.y * marginScale;
				const float mr = text.Margin.z * marginScale;
				const float mb = text.Margin.w * marginScale;

				// Work in unrotated space so margin offsets stay axis-aligned in the rect's local frame (Margin.x insets along local +X regardless of rotation), then rotate the result.
				const Vec2 innerBL{ bl.x * worldScale + ml, bl.y * worldScale + mb };
				const Vec2 innerTR{ tr.x * worldScale - mr, tr.y * worldScale - mt };
				Vec2 inner[4] = {
					Vec2{ innerBL.x, innerBL.y },
					Vec2{ innerTR.x, innerBL.y },
					Vec2{ innerTR.x, innerTR.y },
					Vec2{ innerBL.x, innerTR.y },
				};
				if (rect.Rotation != 0.0f) {
					const Vec2 worldPivot{ pivot.x * worldScale, pivot.y * worldScale };
					for (int i = 0; i < 4; ++i) {
						inner[i] = worldPivot + Rotated(inner[i] - worldPivot, rect.Rotation);
					}
				}

				Gizmo::SetColor(Color(0.95f, 0.95f, 0.95f, 0.85f));
				Gizmo::SetLineWidth(1.0f);
				Gizmo::DrawLine(inner[0], inner[1]);
				Gizmo::DrawLine(inner[1], inner[2]);
				Gizmo::DrawLine(inner[2], inner[3]);
				Gizmo::DrawLine(inner[3], inner[0]);

				const AABB camAABB = m_EditorCamera.GetViewportAABB();
				const float worldPerScreenPx = camAABB.Scale().x / std::max(1.0f, static_cast<float>(m_EditorViewFBO.GetWidth()));
				const float handleHalf = 5.0f * worldPerScreenPx;
				const Vec2 handleSize{ handleHalf * 2.0f, handleHalf * 2.0f };
				const float rectRotDeg = Degrees(rect.Rotation);

				const Vec2 midL{ (inner[0].x + inner[3].x) * 0.5f, (inner[0].y + inner[3].y) * 0.5f };
				const Vec2 midR{ (inner[1].x + inner[2].x) * 0.5f, (inner[1].y + inner[2].y) * 0.5f };
				const Vec2 midB{ (inner[0].x + inner[1].x) * 0.5f, (inner[0].y + inner[1].y) * 0.5f };
				const Vec2 midT{ (inner[2].x + inner[3].x) * 0.5f, (inner[2].y + inner[3].y) * 0.5f };
				Gizmo::DrawSquare(midL, handleSize, rectRotDeg);
				Gizmo::DrawSquare(midR, handleSize, rectRotDeg);
				Gizmo::DrawSquare(midB, handleSize, rectRotDeg);
				Gizmo::DrawSquare(midT, handleSize, rectRotDeg);
			}
		}

		if (componentGizmosEnabled) {
			if (auto* app = Application::GetInstance()) {
				if (auto* sm = app->GetSceneManager()) {
					Entity selected = scene.GetEntity(m_SelectedEntity);
					sm->GetComponentRegistry().ForEachComponentInfo(
						[&](const std::type_index&, const ComponentInfo& info) {
							// Always-draw components are handled in DrawAlwaysVisibleComponentGizmos.
							if (info.drawEditorGizmo && !info.drawEditorGizmoAlways
								&& info.has && info.has(selected)) {
								info.drawEditorGizmo(selected);
							}
						});
				}
			}
		}

		Gizmo::SetLayer(previousLayer);
		Gizmo::SetColor(previousColor);
		Gizmo::SetLineWidth(previousLineWidth);
	}

	void ImGuiEditorLayer::TickParticlePreview(Scene& scene) {
		// Editor-only preview path. In play mode the ParticleUpdateSystem
		// owns the per-frame tick across every ParticleSystem2DComponent,
		// so bailing here avoids a double-step on the selected entity.
		if (Application::GetIsPlaying()) {
			return;
		}

		// Mirror RenderEditorView's prefab-edit override so the preview
		// targets the detached prefab scene when one is being edited and
		// never leaks ticks back onto the main scene's components.
		Scene* renderScene = IsInPrefabEditMode() ? m_PrefabEditScene.get() : &scene;
		if (!renderScene) {
			return;
		}
		const std::uint64_t renderSceneId = static_cast<std::uint64_t>(renderScene->GetSceneId());
		const bool selectedParticleSystem = m_SelectedEntity != entt::null
			&& renderScene->IsValid(m_SelectedEntity)
			&& renderScene->HasComponent<ParticleSystem2DComponent>(m_SelectedEntity);

		if (m_ParticlePreviewEntity != entt::null
			&& (m_ParticlePreviewSceneId != renderSceneId
				|| !selectedParticleSystem
				|| m_SelectedEntity != m_ParticlePreviewEntity))
		{
			if (m_ParticlePreviewSceneId == renderSceneId
				&& renderScene->IsValid(m_ParticlePreviewEntity)
				&& renderScene->HasComponent<ParticleSystem2DComponent>(m_ParticlePreviewEntity))
			{
				auto& previousParticleSystem = renderScene->GetComponent<ParticleSystem2DComponent>(m_ParticlePreviewEntity);
				if (previousParticleSystem.IsEmitting() || previousParticleSystem.IsSimulating()) {
					previousParticleSystem.Stop();
					renderScene->MarkDirty();
				}
			}

			m_ParticlePreviewEntity = entt::null;
			m_ParticlePreviewSceneId = 0;
		}

		if (!selectedParticleSystem) {
			return;
		}

		auto& particleSystem = renderScene->GetComponent<ParticleSystem2DComponent>(m_SelectedEntity);
		if (!particleSystem.IsEmitting() && !particleSystem.IsSimulating()) {
			if (m_ParticlePreviewEntity == m_SelectedEntity && m_ParticlePreviewSceneId == renderSceneId) {
				m_ParticlePreviewEntity = entt::null;
				m_ParticlePreviewSceneId = 0;
			}
			return;
		}

		m_ParticlePreviewEntity = m_SelectedEntity;
		m_ParticlePreviewSceneId = renderSceneId;

		// Unscaled dt: editor preview ignores TimeScale so designers see
		// the effect at its authored cadence regardless of debug slow-mo.
		auto* app = Application::GetInstance();
		const float dt = app ? app->GetTime().GetDeltaTimeUnscaled() : 0.0f;
		particleSystem.PreviewUpdate(dt);
	}

	void ImGuiEditorLayer::RenderEditorView(Scene& scene) {
		// NoScrollbar/NoScrollWithMouse: InvisibleButton overlays set off-panel cursor positions that extend CursorMaxPos and trigger a spurious scrollbar.
		m_IsEditorViewActive = ImGui::Begin("Editor View", nullptr,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		if (!m_IsEditorViewActive) {
			m_IsEditorViewHovered = false;
			m_IsEditorViewFocused = false;
			ImGui::End();
			return;
		}

		{
			// Vertical separator between toolbar button groups, to set them apart.
			auto vsep = []() {
				ImGui::SameLine();
				ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
			};

			constexpr const char* k_DrawModeLabels[] = { "Default", "Triangle", "Mixed" };
			const int currentIndex = static_cast<int>(m_EditorViewDrawMode);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Drawmode:");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(120.0f);
			if (ImGui::BeginCombo("##EditorViewDrawMode", k_DrawModeLabels[currentIndex])) {
				for (int i = 0; i < IM_ARRAYSIZE(k_DrawModeLabels); ++i) {
					const bool selected = (i == currentIndex);
					if (ImGui::Selectable(k_DrawModeLabels[i], selected)) {
						m_EditorViewDrawMode = static_cast<EditorViewDrawMode>(i);
					}
					if (selected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Editor View draw mode (Default / Triangle wireframe / Mixed overlay)");
			}

			vsep();
			ImGui::SameLine();
			{
				const bool active = m_ShowGizmos;
				if (active) {
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				}
				if (ImGui::Button("Gizmos##EditorView")) {
					m_ShowGizmos = !m_ShowGizmos;
				}
				if (active) {
					ImGui::PopStyleColor();
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Toggle gizmos in the Editor View");
				}
			}

			vsep();
			// Gizmo mode: None / Move / Rotate / Scale (icon buttons, text fallback).
			{
				struct GizmoModeButton { const char* Label; const char* Icon; const char* Tooltip; EditorGizmoMode Mode; };
				const GizmoModeButton modeButtons[] = {
					{ "None##GizmoMode",   nullptr,    "No gizmo",          EditorGizmoMode::None },
					{ "Move##GizmoMode",   "move",     "Move (translate)",  EditorGizmoMode::Translate },
					{ "Rotate##GizmoMode", "rotation", "Rotate",            EditorGizmoMode::Rotate },
					{ "Scale##GizmoMode",  "scale",    "Scale",             EditorGizmoMode::Scale },
				};
				for (const GizmoModeButton& b : modeButtons) {
					ImGui::SameLine();
					const bool active = (m_GizmoMode == b.Mode);
					if (active) {
						ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
					}
					const uint64_t icon = b.Icon ? EditorIcons::Get(b.Icon, 16) : 0;
					bool clicked;
					if (icon) {
						const float side = ImGui::GetFontSize();
						clicked = ImGui::ImageButton(b.Label,
							static_cast<ImTextureID>(static_cast<intptr_t>(icon)),
							ImVec2(side, side), ImVec2(0, 1), ImVec2(1, 0));
					} else {
						clicked = ImGui::Button(b.Label);
					}
					if (clicked) {
						m_GizmoMode = b.Mode;
					}
					if (active) {
						ImGui::PopStyleColor();
					}
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", b.Tooltip);
					}
				}
			}

			vsep();
			ImGui::SameLine();
			{
				const bool active = m_ShowPostProcessing;
				if (active) {
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				}
				// Icon button; falls back to text if the asset can't be loaded. Image side == font size so the button matches the adjacent combo/Gizmos frame height.
				const uint64_t ppIcon = EditorIcons::Get("post_processing", 16);
				bool toggled;
				if (ppIcon) {
					const float side = ImGui::GetFontSize();
					toggled = ImGui::ImageButton("##PostProcessingEditorView",
						static_cast<ImTextureID>(static_cast<intptr_t>(ppIcon)),
						ImVec2(side, side), ImVec2(0, 1), ImVec2(1, 0));
				} else {
					toggled = ImGui::Button("Post Processing Effects##EditorView");
				}
				if (toggled) {
					m_ShowPostProcessing = !m_ShowPostProcessing;
				}
				if (active) {
					ImGui::PopStyleColor();
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Toggle post-processing effects (bloom, vignette, ...) in the Editor View");
				}
			}

			vsep();
			// Grid-snapping toggle + grid-size settings popup.
			ImGui::SameLine();
			{
				const bool snapActive = EditorPreferences::GetGridSnapEnabled();
				if (snapActive) {
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				}
				const uint64_t gridIcon = EditorIcons::Get("grid", 16);
				bool snapToggled;
				if (gridIcon) {
					const float side = ImGui::GetFontSize();
					snapToggled = ImGui::ImageButton("##GridSnapEditorView",
						static_cast<ImTextureID>(static_cast<intptr_t>(gridIcon)),
						ImVec2(side, side), ImVec2(0, 1), ImVec2(1, 0));
				} else {
					snapToggled = ImGui::Button("Snap##EditorView");
				}
				if (snapToggled) {
					EditorPreferences::SetGridSnapEnabled(!EditorPreferences::GetGridSnapEnabled());
				}
				if (snapActive) {
					ImGui::PopStyleColor();
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Toggle grid snapping for entities dragged in the Editor View");
				}

				ImGui::SameLine(0.0f, 2.0f);
				if (ImGui::ArrowButton("##GridSnapSettings", ImGuiDir_Down)) {
					ImGui::OpenPopup("##GridSnapPopup");
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Grid snapping settings");
				}

				if (ImGui::BeginPopup("##GridSnapPopup")) {
					bool enabled = EditorPreferences::GetGridSnapEnabled();
					if (ImGui::Checkbox("Snap to grid", &enabled)) {
						EditorPreferences::SetGridSnapEnabled(enabled);
					}

					bool linked = EditorPreferences::GetGridSnapLinkXY();
					float gridX = EditorPreferences::GetGridSizeX();
					float gridY = EditorPreferences::GetGridSizeY();

					ImGui::TextUnformatted("Grid Size");

					// Link toggle: chain icon if present, else a labelled button.
					if (linked) {
						ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
					}
					const uint64_t linkIcon = EditorIcons::Get("link", 16);
					bool linkToggled;
					if (linkIcon) {
						const float side = ImGui::GetFontSize();
						linkToggled = ImGui::ImageButton("##GridSnapLink",
							static_cast<ImTextureID>(static_cast<intptr_t>(linkIcon)),
							ImVec2(side, side), ImVec2(0, 1), ImVec2(1, 0));
					} else {
						linkToggled = ImGui::Button(linked ? "Linked##GridSnapLink" : "Unlinked##GridSnapLink");
					}
					if (linked) {
						ImGui::PopStyleColor();
					}
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("Link X and Y so they share one grid size");
					}
					if (linkToggled) {
						linked = !linked;
						EditorPreferences::SetGridSnapLinkXY(linked);
						if (linked) {
							EditorPreferences::SetGridSizeY(EditorPreferences::GetGridSizeX());
						}
					}

					ImGui::SameLine();
					ImGui::SetNextItemWidth(70.0f);
					if (ImGui::DragFloat("X##GridSizeX", &gridX, 0.05f,
							EditorPreferences::k_MinGridSize, 1000.0f, "%.3f")) {
						EditorPreferences::SetGridSizeX(gridX);
						if (linked) {
							EditorPreferences::SetGridSizeY(gridX);
						}
					}

					ImGui::SameLine();
					ImGui::BeginDisabled(linked);
					ImGui::SetNextItemWidth(70.0f);
					if (ImGui::DragFloat("Y##GridSizeY", &gridY, 0.05f,
							EditorPreferences::k_MinGridSize, 1000.0f, "%.3f")) {
						EditorPreferences::SetGridSizeY(gridY);
					}
					ImGui::EndDisabled();

					ImGui::Separator();

					// Rotation snap: rotate in fixed-degree steps (gizmo Rotate mode).
					bool rotSnap = EditorPreferences::GetRotationSnapEnabled();
					if (ImGui::Checkbox("Snap rotation", &rotSnap)) {
						EditorPreferences::SetRotationSnapEnabled(rotSnap);
					}
					ImGui::SameLine();
					float rotDeg = EditorPreferences::GetRotationSnapDegrees();
					ImGui::SetNextItemWidth(70.0f);
					if (ImGui::DragFloat("##RotationSnap", &rotDeg, 0.5f,
							EditorPreferences::k_MinRotationSnap, 360.0f, "%.1f")) {
						EditorPreferences::SetRotationSnapDegrees(rotDeg);
					}

					// Scale snap: scale in fixed factor steps on X/Y (gizmo Scale mode).
					bool sclSnap = EditorPreferences::GetScaleSnapEnabled();
					if (ImGui::Checkbox("Snap scale", &sclSnap)) {
						EditorPreferences::SetScaleSnapEnabled(sclSnap);
					}
					ImGui::SameLine();
					float sclInc = EditorPreferences::GetScaleSnap();
					ImGui::SetNextItemWidth(70.0f);
					if (ImGui::DragFloat("##ScaleSnap", &sclInc, 0.01f,
							EditorPreferences::k_MinScaleSnap, 100.0f, "%.3f")) {
						EditorPreferences::SetScaleSnap(sclInc);
					}

					ImGui::EndPopup();
				}
			}
		}

		Scene* renderScene = IsInPrefabEditMode() ? m_PrefabEditScene.get() : &scene;

		const ImVec2 viewportSize = ImGui::GetContentRegionAvail();
		const int fbW = static_cast<int>(viewportSize.x);
		const int fbH = static_cast<int>(viewportSize.y);

		if (fbW > 0 && fbH > 0) {

			m_EditorViewFBO.Recreate(fbW, fbH);
			m_EditorCamera.SetViewportSize(fbW, fbH);

			if (m_EditorViewFBO.IsValid()) {

				auto* app = Application::GetInstance();
				if (app) {
					auto& input = app->GetInput();
					float dt = app->GetTime().GetDeltaTimeUnscaled();

					Vec2 mouseDelta = { 0.0f, 0.0f };
					if (m_IsEditorViewHovered && input.GetMouse(MouseButton::Middle)) {
						mouseDelta = input.GetMouseDelta();
					}
					float scroll = m_IsEditorViewHovered ? input.ScrollValue() : 0.0f;

					// Any user-initiated camera input cancels an in-flight focus
					// lerp; otherwise next frame's UpdateEditorCameraFocus would
					// pull the camera right back to the focus target.
					if (m_EditorCameraFocusActive
						&& (scroll != 0.0f || mouseDelta.x != 0.0f || mouseDelta.y != 0.0f)) {
						m_EditorCameraFocusActive = false;
					}

					m_EditorCamera.Update(dt, m_IsEditorViewHovered, mouseDelta, scroll);
				}

				glm::mat4 vp = m_EditorCamera.GetViewProjectionMatrix();
				AABB viewAABB = m_EditorCamera.GetViewportAABB();
				Gizmo::SetViewportAABBOverride(viewAABB);
				// Faint grid overlay aligned to the snap spacing, shown while snapping is on.
				// Grid overlay also appears while Left Ctrl is held over the view, so the
				// temporary Ctrl-snap has a visible grid to snap to.
				if (EditorPreferences::GetGridSnapEnabled()
					|| (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && m_IsEditorViewHovered)) {
					DrawSnapGrid(viewAABB);
				}
				DrawWorldAxes(viewAABB);
				DrawEditorComponentGizmos(*renderScene, m_ShowGizmos);

				static const Color k_EditorClearColor(0.18f, 0.18f, 0.20f, 1.0f);
				const Color k_PrefabClearColor(k_PrefabEditClearR, k_PrefabEditClearG, k_PrefabEditClearB, 1.0f);
				const Color& clearColor = IsInPrefabEditMode() ? k_PrefabClearColor : k_EditorClearColor;
				// uiInWorldSpace=true: UI joins sprites and gizmos in
				// the editor camera's world space so the user can pan
				// and zoom around the UI like any scene object.
				// The Editor View honours the "Post Processing Effects" toolbar toggle:
				// flip the renderer's master PP gate off for this pass so scene editing
				// isn't tinted by bloom/vignette/etc, then restore it immediately so
				// Game View (the real game preview) and the runtime keep their effects.
				auto* ppRenderer = Application::GetInstance() ? Application::GetInstance()->GetRenderer2D() : nullptr;
				if (ppRenderer) ppRenderer->SetPostProcessingEnabled(m_ShowPostProcessing);
				RenderSceneIntoFBO(m_EditorViewFBO, *renderScene, vp, viewAABB,
					true, false, clearColor, IsInPrefabEditMode(), true, m_EditorViewDrawMode,
					m_ShowGizmos ? GizmoLayerMask::All : GizmoLayerMask::EditorOnly);
				if (ppRenderer) ppRenderer->SetPostProcessingEnabled(true);
				Gizmo::ClearViewportAABBOverride();

				ImGui::Image(
					static_cast<ImTextureID>(static_cast<intptr_t>(m_EditorViewFBO.GetColorTextureBackendId())),
					viewportSize);

				ImVec2 imageTopLeft = ImGui::GetItemRectMin();

				if (m_SelectedEntity != entt::null
					&& renderScene->IsValid(m_SelectedEntity)
					&& renderScene->HasComponent<ParticleSystem2DComponent>(m_SelectedEntity))
				{
					auto& particleSystem = renderScene->GetComponent<ParticleSystem2DComponent>(m_SelectedEntity);
					const ImGuiWindowFlags overlayFlags =
						ImGuiWindowFlags_NoSavedSettings |
						ImGuiWindowFlags_NoScrollbar |
						ImGuiWindowFlags_NoScrollWithMouse |
						ImGuiWindowFlags_NoNav;
					const ImGuiStyle& style = ImGui::GetStyle();
					const float spacing = style.ItemSpacing.x;
					const ImVec2 overlaySize{
						64.0f + 72.0f + 64.0f + spacing * 2.0f + style.WindowPadding.x * 2.0f,
						ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f
					};
					const ImVec2 overlayPos{
						std::max(imageTopLeft.x + 8.0f, imageTopLeft.x + viewportSize.x - overlaySize.x - 12.0f),
						std::max(imageTopLeft.y + 8.0f, imageTopLeft.y + viewportSize.y - overlaySize.y - 12.0f)
					};
					ImGui::SetCursorScreenPos(overlayPos);
					ImGui::SetNextWindowBgAlpha(0.86f);
					if (ImGui::BeginChild("##ParticleSystem2DViewportControls", overlaySize, ImGuiChildFlags_Borders, overlayFlags)) {
						const bool isRunning = particleSystem.IsEmitting() || particleSystem.IsSimulating();
						const char* playPauseLabel = isRunning ? "Pause" : "Play";
						if (ImGui::Button(playPauseLabel, ImVec2(64.0f, 0.0f))) {
							if (isRunning) {
								particleSystem.Pause();
							}
							else {
								particleSystem.Play();
							}
							renderScene->MarkDirty();
						}
						ImGui::SameLine();
						if (ImGui::Button("Restart", ImVec2(72.0f, 0.0f))) {
							particleSystem.Restart();
							renderScene->MarkDirty();
						}
						ImGui::SameLine();
						if (ImGui::Button("Stop", ImVec2(64.0f, 0.0f))) {
							particleSystem.Stop();
							renderScene->MarkDirty();
						}
					}
					ImGui::EndChild();
				}

				if (m_SelectedEntity != entt::null
					&& renderScene->IsValid(m_SelectedEntity)
					&& renderScene->HasComponent<RectTransform2DComponent>(m_SelectedEntity))
				{
					auto& rect = renderScene->GetComponent<RectTransform2DComponent>(m_SelectedEntity);

					const float worldScale = GuiRenderer::ComputeWorldUIPixelScale();
					const Vec2 bl = rect.GetBottomLeft();
					const Vec2 tr = rect.GetTopRight();
					const Vec2 pivot = rect.ResolvedValid ? rect.ResolvedPivot
						: Vec2{ (bl.x + tr.x) * 0.5f, (bl.y + tr.y) * 0.5f };

					Vec2 handles[4] = {
						Vec2{ bl.x * worldScale, ((bl.y + tr.y) * 0.5f) * worldScale },
						Vec2{ tr.x * worldScale, ((bl.y + tr.y) * 0.5f) * worldScale },
						Vec2{ ((bl.x + tr.x) * 0.5f) * worldScale, bl.y * worldScale },
						Vec2{ ((bl.x + tr.x) * 0.5f) * worldScale, tr.y * worldScale },
					};
					if (rect.Rotation != 0.0f) {
						const Vec2 worldPivot{ pivot.x * worldScale, pivot.y * worldScale };
						for (int i = 0; i < 4; ++i) {
							handles[i] = worldPivot + Rotated(handles[i] - worldPivot, rect.Rotation);
						}
					}

					auto worldToScreen = [&](const Vec2& w, ImVec2& outScreen) -> bool {
						glm::vec4 wp(w.x, w.y, 0.0f, 1.0f);
						glm::vec4 cp = vp * wp;
						if (cp.w == 0.0f) return false;
						const float ndcX = cp.x / cp.w;
						const float ndcY = cp.y / cp.w;
						outScreen.x = (ndcX * 0.5f + 0.5f) * viewportSize.x;
						outScreen.y = (1.0f - (ndcY * 0.5f + 0.5f)) * viewportSize.y;
						return true;
					};

					const AABB camAABB = m_EditorCamera.GetViewportAABB();
					const float worldPerScreenPxX = camAABB.Scale().x / std::max(1.0f, viewportSize.x);
					const float worldPerScreenPxY = camAABB.Scale().y / std::max(1.0f, viewportSize.y);
					const float scaleX = worldScale * std::max(0.01f, std::abs(rect.Scale.x));
					const float scaleY = worldScale * std::max(0.01f, std::abs(rect.Scale.y));
					constexpr const char* kButtonIds[4] = {
						"##RectSizeL", "##RectSizeR",
						"##RectSizeB", "##RectSizeT",
					};
					constexpr float kHandleSizePx = 12.0f;
					const float kHalf = kHandleSizePx * 0.5f;

					for (int i = 0; i < 4; ++i) {
						ImVec2 screen;
						if (!worldToScreen(handles[i], screen)) continue;

						const ImVec2 btnTL(imageTopLeft.x + screen.x - kHalf,
							imageTopLeft.y + screen.y - kHalf);
						ImGui::SetCursorScreenPos(btnTL);
						ImGui::InvisibleButton(kButtonIds[i], ImVec2(kHandleSizePx, kHandleSizePx));

						if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
							ImGui::SetMouseCursor(
								(i < 2) ? ImGuiMouseCursor_ResizeEW
										: ImGuiMouseCursor_ResizeNS);
						}
						if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
							const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
							const float localDx = (scaleX > 0.0f) ? (mouseDelta.x * worldPerScreenPxX) / scaleX : 0.0f;
							const float localDy = (scaleY > 0.0f) ? (-mouseDelta.y * worldPerScreenPxY) / scaleY : 0.0f;

							switch (i) {
							case 0: {
								float delta = localDx;
								if (rect.SizeDelta.x - delta < 1.0f) delta = rect.SizeDelta.x - 1.0f;
								rect.SizeDelta.x -= delta;
								rect.AnchoredPosition.x += delta * (1.0f - rect.Pivot.x);
								break;
							}
							case 1: {
								float delta = localDx;
								if (rect.SizeDelta.x + delta < 1.0f) delta = 1.0f - rect.SizeDelta.x;
								rect.SizeDelta.x += delta;
								rect.AnchoredPosition.x += delta * rect.Pivot.x;
								break;
							}
							case 2: {
								float delta = localDy;
								if (rect.SizeDelta.y - delta < 1.0f) delta = rect.SizeDelta.y - 1.0f;
								rect.SizeDelta.y -= delta;
								rect.AnchoredPosition.y += delta * (1.0f - rect.Pivot.y);
								break;
							}
							case 3: {
								float delta = localDy;
								if (rect.SizeDelta.y + delta < 1.0f) delta = 1.0f - rect.SizeDelta.y;
								rect.SizeDelta.y += delta;
								rect.AnchoredPosition.y += delta * rect.Pivot.y;
								break;
							}
							default:
								break;
							}
							renderScene->MarkDirty();
						}
					}
				}

				if (m_ShowGizmos) {
				const float iconSize = 24.0f;
				const float halfIcon = iconSize * 0.5f;
				auto camView = renderScene->GetRegistry().view<Camera2DComponent, Transform2DComponent>();
				for (auto [ent, cam, transform] : camView.each()) {
					glm::vec4 worldPos(transform.Position.x, transform.Position.y, 0.0f, 1.0f);
					glm::vec4 clipPos = vp * worldPos;
					if (clipPos.w == 0.0f) continue;

					float ndcX = clipPos.x / clipPos.w;
					float ndcY = clipPos.y / clipPos.w;
					float screenX = (ndcX * 0.5f + 0.5f) * viewportSize.x;
					float screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * viewportSize.y;

					if (screenX < -halfIcon || screenX > viewportSize.x + halfIcon ||
						screenY < -halfIcon || screenY > viewportSize.y + halfIcon) {
						continue;
					}

					uint64_t camIcon = EditorIcons::Get("camera", 24);
					if (!camIcon) {
						continue;
					}

					ImVec2 iconPos(imageTopLeft.x + screenX - halfIcon, imageTopLeft.y + screenY - halfIcon);
					ImGui::GetWindowDrawList()->AddImage(
						static_cast<ImTextureID>(static_cast<intptr_t>(camIcon)),
						iconPos,
						ImVec2(iconPos.x + iconSize, iconPos.y + iconSize),
						ImVec2(0, 1), ImVec2(1, 0));
				}
				}

				if (m_SelectedEntity != entt::null
					&& m_ShowGizmos
					&& renderScene->IsValid(m_SelectedEntity)
					&& renderScene->HasComponent<TextRendererComponent>(m_SelectedEntity)
					&& renderScene->HasComponent<RectTransform2DComponent>(m_SelectedEntity))
				{
					auto& text = renderScene->GetComponent<TextRendererComponent>(m_SelectedEntity);
					auto& rect = renderScene->GetComponent<RectTransform2DComponent>(m_SelectedEntity);

					const float worldScale = GuiRenderer::ComputeWorldUIPixelScale();
					const Vec2 bl = rect.GetBottomLeft();
					const Vec2 tr = rect.GetTopRight();
					const Vec2 pivot = rect.ResolvedValid ? rect.ResolvedPivot
						: Vec2{ (bl.x + tr.x) * 0.5f, (bl.y + tr.y) * 0.5f };
					const float marginScale = worldScale * std::max(0.01f, std::abs(rect.Scale.x));

					// Inner-rect midpoints in world space (matches the
					// gizmo block in DrawEditorComponentGizmos so the
					// invisible buttons sit exactly on the painted squares).
					const float innerLx = bl.x * worldScale + text.Margin.x * marginScale;
					const float innerRx = tr.x * worldScale - text.Margin.z * marginScale;
					const float innerBy = bl.y * worldScale + text.Margin.w * marginScale;
					const float innerTy = tr.y * worldScale - text.Margin.y * marginScale;
					const float midLx = innerLx, midLy = (innerBy + innerTy) * 0.5f;
					const float midRx = innerRx, midRy = (innerBy + innerTy) * 0.5f;
					const float midBx = (innerLx + innerRx) * 0.5f, midBy = innerBy;
					const float midTx = (innerLx + innerRx) * 0.5f, midTy = innerTy;

					Vec2 handles[4] = {
						Vec2{ midLx, midLy }, // 0 = Left
						Vec2{ midRx, midRy }, // 1 = Right
						Vec2{ midBx, midBy }, // 2 = Bottom
						Vec2{ midTx, midTy }, // 3 = Top
					};
					if (rect.Rotation != 0.0f) {
						const Vec2 worldPivot{ pivot.x * worldScale, pivot.y * worldScale };
						for (int i = 0; i < 4; ++i) {
							handles[i] = worldPivot + Rotated(handles[i] - worldPivot, rect.Rotation);
						}
					}

					// World→screen for each handle (same projection used
					// by the camera-icon overlay above).
					auto worldToScreen = [&](const Vec2& w, ImVec2& outScreen) -> bool {
						glm::vec4 wp(w.x, w.y, 0.0f, 1.0f);
						glm::vec4 cp = vp * wp;
						if (cp.w == 0.0f) return false;
						const float ndcX = cp.x / cp.w;
						const float ndcY = cp.y / cp.w;
						outScreen.x = (ndcX * 0.5f + 0.5f) * viewportSize.x;
						outScreen.y = (1.0f - (ndcY * 0.5f + 0.5f)) * viewportSize.y;
						return true;
					};

					// World units per screen pixel — needed to convert the
					// drag delta back into the rect's local pixel-domain
					// margin units.
					const AABB camAABB = m_EditorCamera.GetViewportAABB();
					const float worldPerScreenPxX = camAABB.Scale().x / std::max(1.0f, viewportSize.x);
					const float worldPerScreenPxY = camAABB.Scale().y / std::max(1.0f, viewportSize.y);

					constexpr const char* kButtonIds[4] = {
						"##TextMarginL", "##TextMarginR",
						"##TextMarginB", "##TextMarginT",
					};
					constexpr float kHandleSizePx = 12.0f;
					const float kHalf = kHandleSizePx * 0.5f;

					// Do NOT restore cursor after this loop: restoring with SetCursorScreenPos re-sets IsSetPos, causing ImGui::End to assert (cursor.y > max.y).

					for (int i = 0; i < 4; ++i) {
						ImVec2 screen;
						if (!worldToScreen(handles[i], screen)) continue;

						const ImVec2 btnTL(imageTopLeft.x + screen.x - kHalf,
							imageTopLeft.y + screen.y - kHalf);
						ImGui::SetCursorScreenPos(btnTL);
						ImGui::InvisibleButton(kButtonIds[i], ImVec2(kHandleSizePx, kHandleSizePx));

						if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
							// Cursor hint matching the axis being dragged.
							ImGui::SetMouseCursor(
								(i < 2) ? ImGuiMouseCursor_ResizeEW
										: ImGuiMouseCursor_ResizeNS);
						}
						if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
							const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
							const float worldDx = mouseDelta.x * worldPerScreenPxX;
							const float worldDy = -mouseDelta.y * worldPerScreenPxY;
							// World-to-rect-pixel: divide by marginScale.
							const float pixDx = (marginScale > 0.0f) ? worldDx / marginScale : 0.0f;
							const float pixDy = (marginScale > 0.0f) ? worldDy / marginScale : 0.0f;
							switch (i) {
							case 0: text.Margin.x += pixDx; break; // Left   → drag right grows left margin
							case 1: text.Margin.z -= pixDx; break; // Right  → drag right SHRINKS right margin
							case 2: text.Margin.w += pixDy; break; // Bottom → drag up grows bottom margin
							case 3: text.Margin.y -= pixDy; break; // Top    → drag up SHRINKS top margin
							}
							if (renderScene) renderScene->MarkDirty();
						}
					}
					// (No cursor restore — see comment above the loop.)
				}

				// -- ImGuizmo manipulation gizmo for the primary selection --
				bool gizmoActive = false;
				if (m_GizmoMode != EditorGizmoMode::None
					&& m_SelectedEntity != entt::null
					&& renderScene->IsValid(m_SelectedEntity)
					&& renderScene->HasComponent<Transform2DComponent>(m_SelectedEntity))
				{
					TransformHierarchySystem::Propagate(*renderScene);
					const Transform2DComponent& primary = renderScene->GetComponent<Transform2DComponent>(m_SelectedEntity);

					// The gizmo sits at the centroid of the selection so it's centred on the
					// whole group. Translate always uses the centroid; Rotate/Scale use it too
					// once MULTIPLE entities are selected (group transform about the centre).
					Vec2 gizmoCenter{ primary.Position.x, primary.Position.y };
					Vec2 centerSum{ 0.0f, 0.0f };
					int centerCount = 0;
					for (EntityHandle handle : GetSelectedEntities(*renderScene)) {
						Transform2DComponent* selectedTransform = nullptr;
						if (renderScene->TryGetComponent<Transform2DComponent>(handle, selectedTransform) && selectedTransform) {
							centerSum.x += selectedTransform->Position.x;
							centerSum.y += selectedTransform->Position.y;
							++centerCount;
						}
					}
					if (centerCount > 0) {
						gizmoCenter = { centerSum.x / static_cast<float>(centerCount),
										centerSum.y / static_cast<float>(centerCount) };
					}
					const bool groupTransform = centerCount > 1;

					// Multi-select uses an identity-oriented gizmo at the centroid (all modes).
					// A single-entity Move carries the entity's rotation so the move arrows
					// align to it; single Rotate/Scale carries the full transform.
					glm::mat4 model;
					if (groupTransform) {
						model = glm::translate(glm::mat4(1.0f), glm::vec3(gizmoCenter.x, gizmoCenter.y, 0.0f));
					}
					else if (m_GizmoMode == EditorGizmoMode::Translate) {
						model = glm::translate(glm::mat4(1.0f), glm::vec3(primary.Position.x, primary.Position.y, 0.0f)) *
							glm::rotate(glm::mat4(1.0f), primary.Rotation, glm::vec3(0.0f, 0.0f, 1.0f));
					}
					else {
						model = glm::translate(glm::mat4(1.0f), glm::vec3(primary.Position.x, primary.Position.y, 0.0f)) *
							glm::rotate(glm::mat4(1.0f), primary.Rotation, glm::vec3(0.0f, 0.0f, 1.0f)) *
							glm::scale(glm::mat4(1.0f), glm::vec3(primary.Scale.x, primary.Scale.y, 1.0f));
					}

					// ImGuizmo needs the camera off the entity plane (z=0) or its move-plane
					// math degenerates; pull it back in z. The x/y ortho extents match the
					// editor camera so the gizmo stays aligned with the sprites.
					const AABB camAABB = m_EditorCamera.GetViewportAABB();
					const float halfW = (camAABB.Max.x - camAABB.Min.x) * 0.5f;
					const float halfH = (camAABB.Max.y - camAABB.Min.y) * 0.5f;
					const Vec2 camPos = m_EditorCamera.GetPosition();
					const glm::mat4 gizmoView = glm::translate(glm::mat4(1.0f), glm::vec3(-camPos.x, -camPos.y, -10.0f));
					const glm::mat4 gizmoProj = glm::ortho(-halfW, halfW, -halfH, halfH, 0.01f, 1000.0f);

					// Translate uses full TRANSLATE (X|Y|Z): the Z bit activates ImGuizmo's
					// centre screen-move handle (the white circle) for free XY movement. The
					// degenerate Z axis is masked and the XY plane quad (blue square) is dropped
					// below, leaving just the X/Y arrows + the centre free-move circle.
					ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
					// Translate uses LOCAL space so a single rotated entity's move arrows follow
					// its rotation; group Translate has identity orientation so LOCAL == WORLD.
					ImGuizmo::MODE gizmoSpace = ImGuizmo::LOCAL;
					if (m_GizmoMode == EditorGizmoMode::Rotate) {
						op = ImGuizmo::ROTATE_Z;
						gizmoSpace = ImGuizmo::WORLD;
					}
					else if (m_GizmoMode == EditorGizmoMode::Scale) {
						// Full SCALE (X|Y|Z) — the Z bit is what makes ImGuizmo's centre
						// handle (MT_SCALE_XYZ) live, giving an even X+Y scale grip alongside
						// the per-axis ones. Z itself is degenerate in our 2D ortho view
						// (hidden) and ignored on apply (we only read the X/Y columns).
						op = ImGuizmo::SCALE;
						gizmoSpace = ImGuizmo::LOCAL;
					}

					// Required once per frame: resets ImGuizmo's per-frame hover state
					// (mbOverGizmoHotspot). Without it the flag latches and dragging dies.
					ImGuizmo::BeginFrame();
					ImGuizmo::SetOrthographic(true);
					ImGuizmo::SetDrawlist();
					ImGuizmo::SetRect(imageTopLeft.x, imageTopLeft.y, viewportSize.x, viewportSize.y);
					ImGuizmo::AllowAxisFlip(false);
					// Suppress ImGuizmo's degenerate-Z handles in our 2D view. In Translate and
					// Scale the Z axis is edge-on; masking it stops its invisible centre-grab
					// (Translate's MT_MOVE_SCREEN and Scale's MT_SCALE_XYZ ignore the mask, so the
					// centre handles stay live). Rotate keeps Z — it rotates about it. In Translate
					// we also raise the plane-area limit so the XY plane quad (the blue square) is
					// dropped entirely; free movement is the centre screen-move circle instead.
					// mAxisMask/mAxisLimit are persistent global state (BeginFrame doesn't clear
					// them), so set both every frame.
					const bool isTranslate = (m_GizmoMode == EditorGizmoMode::Translate);
					ImGuizmo::SetAxisMask(false, false, isTranslate || m_GizmoMode == EditorGizmoMode::Scale);
					ImGuizmo::SetAxisLimit(isTranslate ? 1.0e6f : 0.0025f);

					// Holding Left Ctrl temporarily forces snapping on for the duration of the
					// manipulation; releasing it reverts to the toolbar snap toggles.
					const bool snapOverride = ImGui::IsKeyDown(ImGuiKey_LeftCtrl);

					// Rotation/scale snapping: ImGuizmo snaps the total angle/scale (snap[0] is
					// degrees for Rotate, a factor increment for Scale) — works for single and
					// group manipulation since both read back the snapped result.
					float snapValues[3] = { 0.0f, 0.0f, 0.0f };
					const float* snapPtr = nullptr;
					if (m_GizmoMode == EditorGizmoMode::Rotate
						&& (EditorPreferences::GetRotationSnapEnabled() || snapOverride)) {
						snapValues[0] = snapValues[1] = snapValues[2] = EditorPreferences::GetRotationSnapDegrees();
						snapPtr = snapValues;
					}
					else if (m_GizmoMode == EditorGizmoMode::Scale
						&& (EditorPreferences::GetScaleSnapEnabled() || snapOverride)) {
						snapValues[0] = snapValues[1] = snapValues[2] = EditorPreferences::GetScaleSnap();
						snapPtr = snapValues;
					}
					ImGuizmo::Manipulate(&gizmoView[0][0], &gizmoProj[0][0], op, gizmoSpace, &model[0][0], nullptr, snapPtr);

					gizmoActive = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

					// Reset the per-frame scale accumulator whenever no drag is active so the
					// next group-scale drag starts from a clean total.
					if (!ImGuizmo::IsUsing()) {
						m_GizmoGroupScalePrev = Vec2{ 1.0f, 1.0f };
					}

					if (ImGuizmo::IsUsing()) {
						if (m_GizmoMode == EditorGizmoMode::Translate) {
							// Gizmo is at the centroid: turn its motion into a world delta,
							// re-anchored on the active entity. ApplyGizmoManipulation carries
							// that delta across the whole selection; snap the active entity.
							Vec2 newPos{
								primary.Position.x + (model[3][0] - gizmoCenter.x),
								primary.Position.y + (model[3][1] - gizmoCenter.y)
							};
							if (EditorPreferences::GetGridSnapEnabled() || snapOverride) {
								const float gx = EditorPreferences::GetGridSizeX();
								const float gy = EditorPreferences::GetGridSizeY();
								if (gx > 0.0f) newPos.x = std::round(newPos.x / gx) * gx;
								if (gy > 0.0f) newPos.y = std::round(newPos.y / gy) * gy;
							}
							ApplyGizmoManipulation(*renderScene, newPos, primary.Rotation, primary.Scale);
						}
						else if (groupTransform) {
							// Multi-selection Rotate/Scale: apply the gizmo's motion about the
							// centroid to every selected root (model was passed identity-oriented).
							if (m_GizmoMode == EditorGizmoMode::Rotate) {
								// ImGuizmo advances its rotation origin every frame, so the result's
								// rotation is THIS frame's incremental angle.
								const float deltaAngle = std::atan2(model[0][1], model[0][0]);
								ApplyGroupRotationScale(*renderScene, gizmoCenter, deltaAngle, Vec2{ 1.0f, 1.0f });
							}
							else {
								// ImGuizmo scale is the TOTAL since the drag started; divide by the
								// previous total to get this frame's factor.
								const Vec2 totalScale{
									std::sqrt(model[0][0] * model[0][0] + model[0][1] * model[0][1]),
									std::sqrt(model[1][0] * model[1][0] + model[1][1] * model[1][1])
								};
								const Vec2 factor{
									m_GizmoGroupScalePrev.x != 0.0f ? totalScale.x / m_GizmoGroupScalePrev.x : 1.0f,
									m_GizmoGroupScalePrev.y != 0.0f ? totalScale.y / m_GizmoGroupScalePrev.y : 1.0f
								};
								ApplyGroupRotationScale(*renderScene, gizmoCenter, 0.0f, factor);
								m_GizmoGroupScalePrev = totalScale;
							}
						}
						else {
							const Vec2 newPos{ model[3][0], model[3][1] };
							const float newRot = std::atan2(model[0][1], model[0][0]);
							const Vec2 newScale{
								std::sqrt(model[0][0] * model[0][0] + model[0][1] * model[0][1]),
								std::sqrt(model[1][0] * model[1][0] + model[1][1] * model[1][1])
							};
							ApplyGizmoManipulation(*renderScene, newPos, newRot, newScale);
						}
					}
				}

				// Selection: a left-drag draws a marquee that box-selects every entity it
				// touches; a plain click (press + release without dragging) selects the
				// entity under the cursor, as before.
				const AABB selectCamAABB = m_EditorCamera.GetViewportAABB();
				auto screenToWorld = [&](float sx, float sy) -> Vec2 {
					const float u = (sx - imageTopLeft.x) / std::max(1.0f, viewportSize.x);
					const float v = (sy - imageTopLeft.y) / std::max(1.0f, viewportSize.y);
					return Vec2{
						selectCamAABB.Min.x + u * (selectCamAABB.Max.x - selectCamAABB.Min.x),
						selectCamAABB.Max.y - v * (selectCamAABB.Max.y - selectCamAABB.Min.y)
					};
				};

				// Begin on left-press inside the viewport image (not on a gizmo handle or
				// other active item). The actual selection is deferred to release.
				if (!gizmoActive
					&& ImGui::IsMouseClicked(ImGuiMouseButton_Left)
					&& ImGui::IsWindowHovered()
					&& !ImGui::IsAnyItemActive())
				{
					const ImVec2 mp = ImGui::GetMousePos();
					const float lx = mp.x - imageTopLeft.x;
					const float ly = mp.y - imageTopLeft.y;
					if (lx >= 0.0f && lx < viewportSize.x && ly >= 0.0f && ly < viewportSize.y) {
						m_BoxSelectPending = true;
						m_BoxSelectActive = false;
						m_BoxSelectStartScreen = Vec2{ mp.x, mp.y };
					}
				}

				// Promote to a box-select once the press drags past a small threshold.
				if (m_BoxSelectPending && !m_BoxSelectActive
					&& ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
					m_BoxSelectActive = true;
				}

				// Draw the marquee on top of the viewport image while dragging.
				if (m_BoxSelectActive) {
					const ImVec2 cur = ImGui::GetMousePos();
					const ImVec2 rMin{ std::min(m_BoxSelectStartScreen.x, cur.x), std::min(m_BoxSelectStartScreen.y, cur.y) };
					const ImVec2 rMax{ std::max(m_BoxSelectStartScreen.x, cur.x), std::max(m_BoxSelectStartScreen.y, cur.y) };
					ImDrawList* dl = ImGui::GetWindowDrawList();
					dl->AddRectFilled(rMin, rMax, IM_COL32(90, 145, 230, 40));
					dl->AddRect(rMin, rMax, IM_COL32(120, 170, 240, 220));
				}

				// Resolve on release: box-select if dragged, else single-click select.
				if (m_BoxSelectPending && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
					const bool hasModifier = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;

					if (m_BoxSelectActive) {
						const ImVec2 cur = ImGui::GetMousePos();
						const Vec2 w0 = screenToWorld(m_BoxSelectStartScreen.x, m_BoxSelectStartScreen.y);
						const Vec2 w1 = screenToWorld(cur.x, cur.y);
						const AABB worldRect{
							Vec2{ std::min(w0.x, w1.x), std::min(w0.y, w1.y) },
							Vec2{ std::max(w0.x, w1.x), std::max(w0.y, w1.y) }
						};
						// Bounds reads need current transforms + UI layout.
						TransformHierarchySystem::Propagate(*renderScene);
						ComputeUILayout(*renderScene);
						std::vector<EntityHandle> inBox;
						EntityPicker::PickEntitiesInRect(*renderScene, worldRect, inBox);
						SelectEntitiesInBox(inBox, hasModifier);
					}
					else {
						const ImVec2 mp = ImGui::GetMousePos();
						const float lx = mp.x - imageTopLeft.x;
						const float ly = mp.y - imageTopLeft.y;
						if (lx >= 0.0f && lx < viewportSize.x && ly >= 0.0f && ly < viewportSize.y) {
							const Vec2 worldPoint = screenToWorld(mp.x, mp.y);
							EntityHandle picked = entt::null;
							EntityPicker::TryPickEntity(*renderScene, worldPoint, picked);
							if (picked != entt::null) {
								if (hasModifier) ToggleEntitySelection(picked, -1);
								else SetSingleEntitySelection(picked, -1);
							}
							else if (!hasModifier) {
								ClearEntitySelection();
							}
						}
					}

					m_BoxSelectPending = false;
					m_BoxSelectActive = false;
				}

				// Safety: drop a stale drag if the release was missed (e.g. panel lost focus).
				if (m_BoxSelectPending && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
					m_BoxSelectPending = false;
					m_BoxSelectActive = false;
				}
			}
		}
		else {
			ImGui::TextDisabled("Editor View has no drawable area");
		}

		m_IsEditorViewHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
		m_IsEditorViewFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		ImGui::End();
	}

	void ImGuiEditorLayer::RenderGameView(Scene& scene) {
		m_IsGameViewActive = ImGui::Begin("Game View");

		if (!m_IsGameViewActive) {
			ImGui::End();
			m_IsGameViewFocused = false;
			m_IsGameViewHovered = false;
			ApplicationEditorAccess::SetGameInputEnabled(false);
			return;
		}

		const int aspectPresetIndex = std::clamp(m_GameViewAspectPresetIndex, 0, static_cast<int>(k_AspectRatioPresets.size()) - 1);
		m_GameViewAspectPresetIndex = aspectPresetIndex;
		if (!m_GameViewAspectLoaded) {
			if (IndexProject* project = ProjectManager::GetCurrentProject()) {
				for (int i = 0; i < static_cast<int>(k_AspectRatioPresets.size()); ++i) {
					if (project->GameViewAspect == k_AspectRatioPresets[i].Label) {
						m_GameViewAspectPresetIndex = i;
						break;
					}
				}
			}
			m_GameViewAspectLoaded = true;
		}
		if (!m_GameViewVsyncLoaded) {
			if (IndexProject* project = ProjectManager::GetCurrentProject()) {
				m_GameViewVsync = project->GameViewVsync;
			}
			m_GameViewVsyncLoaded = true;
		}

		ImGui::SetNextItemWidth(140.0f);
		if (ImGui::BeginCombo("##GameViewAspect", k_AspectRatioPresets[m_GameViewAspectPresetIndex].Label)) {
			for (int i = 0; i < static_cast<int>(k_AspectRatioPresets.size()); ++i) {
				const bool selected = (i == m_GameViewAspectPresetIndex);
				if (ImGui::Selectable(k_AspectRatioPresets[i].Label, selected)) {
					m_GameViewAspectPresetIndex = i;
					if (IndexProject* project = ProjectManager::GetCurrentProject()) {
						project->GameViewAspect = k_AspectRatioPresets[i].Label;
						project->Save();
					}
				}
				if (selected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();
		if (ImGui::Checkbox("VSync##GameView", &m_GameViewVsync)) {
			m_GameViewHasRendered = false;
			if (IndexProject* project = ProjectManager::GetCurrentProject()) {
				project->GameViewVsync = m_GameViewVsync;
				project->Save();
			}
		}

		ImGui::SameLine();
		{
			const bool active = m_ShowGameViewStats;
			if (active) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			}
			if (ImGui::Button("Stats##GameView")) {
				m_ShowGameViewStats = !m_ShowGameViewStats;
			}
			if (active) {
				ImGui::PopStyleColor();
			}
		}

		// "Logs" toggle. Sibling to Stats; the log overlay stacks below
		// the stats overlay when both are visible.
		ImGui::SameLine();
		{
			const bool active = m_ShowGameViewLogs;
			if (active) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			}
			if (ImGui::Button("Logs##GameView")) {
				m_ShowGameViewLogs = !m_ShowGameViewLogs;
			}
			if (active) {
				ImGui::PopStyleColor();
			}
		}

		ImGui::Separator();

		const ImVec2 viewportSize = ImGui::GetContentRegionAvail();
		const float targetAspect = k_AspectRatioPresets[m_GameViewAspectPresetIndex].Aspect;

		ImVec2 renderSize = viewportSize;
		if (targetAspect > 0.0f && viewportSize.x > 0.0f && viewportSize.y > 0.0f) {
			const float availableAspect = viewportSize.x / viewportSize.y;
			if (availableAspect > targetAspect) {
				renderSize.x = viewportSize.y * targetAspect;
			}
			else {
				renderSize.y = viewportSize.x / targetAspect;
			}
		}

		const int fbW = std::max(1, static_cast<int>(std::round(renderSize.x)));
		const int fbH = std::max(1, static_cast<int>(std::round(renderSize.y)));

		if (viewportSize.x > 0.0f && viewportSize.y > 0.0f) {
			Camera2DComponent* gameCam = Camera2DComponent::Main();
			if ((gameCam && gameCam->IsValid()) || !m_GameViewFBO.IsValid()) {
				m_GameViewFBO.Recreate(fbW, fbH);
			}

			auto drawNoCameraFallback = [&]() {
				const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
				const ImVec2 canvasMax(canvasMin.x + viewportSize.x, canvasMin.y + viewportSize.y);
				const ImVec2 imageMin(
					canvasMin.x + (viewportSize.x - renderSize.x) * 0.5f,
					canvasMin.y + (viewportSize.y - renderSize.y) * 0.5f);
				const ImVec2 imageMax(imageMin.x + renderSize.x, imageMin.y + renderSize.y);

				ImGui::InvisibleButton("##GameViewCanvas", viewportSize);
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(0, 0, 0, 255));
				if (m_GameViewHasRendered && m_GameViewFBO.IsValid()) {
					drawList->AddImage(
						static_cast<ImTextureID>(static_cast<intptr_t>(m_GameViewFBO.GetColorTextureBackendId())),
						imageMin,
						imageMax);
				}
				drawList->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 40));

				const char* message = "no main camera in scene";
				const ImVec2 textSize = ImGui::CalcTextSize(message);
				drawList->AddText(
					ImVec2(imageMin.x + (renderSize.x - textSize.x) * 0.5f,
						imageMin.y + (renderSize.y - textSize.y) * 0.5f),
					ImGui::GetColorU32(ImGuiCol_TextDisabled),
					message);

				float statsRenderedHeight = 0.0f;
				if (m_ShowGameViewStats) {
					m_GameViewStatsOverlay.RefreshIfDue(fbW, fbH);
					statsRenderedHeight = m_GameViewStatsOverlay.RenderInRect(imageMin, imageMax);
				}
				if (m_ShowGameViewLogs) {
					if (!m_GameViewLogOverlay) {
						m_GameViewLogOverlay = std::make_unique<Index::Diagnostics::LogOverlay>();
					}
					const float logYOffset = statsRenderedHeight > 0.0f
						? statsRenderedHeight + 8.0f
						: 0.0f;
					m_GameViewLogOverlay->RenderInRect(imageMin, imageMax, logYOffset);
				}
			};

			if (m_GameViewFBO.IsValid() && gameCam && gameCam->IsValid()) {
				Viewport* savedViewport = gameCam->GetViewport();
				if (!savedViewport) {
					drawNoCameraFallback();
					m_IsGameViewHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
					m_IsGameViewFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
					ApplicationEditorAccess::SetGameInputEnabled(m_IsGameViewFocused);
					ImGui::End();
					return;
				}
				const int savedW = savedViewport->GetWidth();
				const int savedH = savedViewport->GetHeight();

				// RAII guard: if RenderSceneIntoFBO throws, the camera viewport
				// is still restored before the exception unwinds out of ImGui::End.
				struct ViewportRestoreGuard {
					Viewport* vp;
					int w;
					int h;
					Camera2DComponent* cam;
					~ViewportRestoreGuard() {
						vp->SetSize(w, h);
						cam->UpdateViewport();
					}
				} guard{ savedViewport, savedW, savedH, gameCam };

				savedViewport->SetSize(fbW, fbH);
				gameCam->UpdateViewport();
				glm::mat4 vp = gameCam->GetViewProjectionMatrix();
				AABB viewAABB = gameCam->GetViewportAABB();
				const auto now = std::chrono::steady_clock::now();
				float targetFps = 0.0f;
				const bool appVsyncEnabled = Window::IsVsync();
				if (m_GameViewVsync && appVsyncEnabled) {
					if (auto* window = Application::GetWindow()) {
						const GLFWvidmode* videoMode = window->GetVideomode();
						targetFps = videoMode ? static_cast<float>(videoMode->refreshRate) : 60.0f;
					}
					else {
						targetFps = 60.0f;
					}
				}
				else {
					targetFps = std::max(Application::GetTargetFramerate(), 0.0f);
				}

				bool renderFrame = !m_GameViewHasRendered
					|| m_LastGameViewFbW != fbW
					|| m_LastGameViewFbH != fbH;
				if (!renderFrame && targetFps > 0.0f) {
					const auto frameDuration = std::chrono::duration<double>(1.0 / static_cast<double>(targetFps));
					renderFrame = now - m_LastGameViewRenderTime >= frameDuration;
				}
				else if (targetFps <= 0.0f) {
					renderFrame = true;
				}

				const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
				const ImVec2 canvasMax(canvasMin.x + viewportSize.x, canvasMin.y + viewportSize.y);
				const ImVec2 imageMin(
					canvasMin.x + (viewportSize.x - renderSize.x) * 0.5f,
					canvasMin.y + (viewportSize.y - renderSize.y) * 0.5f);
				const ImVec2 imageMax(imageMin.x + renderSize.x, imageMin.y + renderSize.y);

				// MUST precede RenderSceneIntoFBO: UIRegion pins layout/event coords to the panel rect; subtract mainViewportPos to convert ImGui desktop coords to GLFW window-client space (avoids Y-drift when window is not at desktop Y=0).
				const ImVec2 mainViewportPos = ImGui::GetMainViewport()->Pos;
				Window::SetUIRegion(
					static_cast<int>(imageMin.x - mainViewportPos.x),
					static_cast<int>(imageMin.y - mainViewportPos.y),
					static_cast<int>(renderSize.x),
					static_cast<int>(renderSize.y));

				if (renderFrame) {
					RenderSceneIntoFBO(m_GameViewFBO, scene, vp, viewAABB, true, true, gameCam->GetClearColor());
					m_LastGameViewRenderTime = now;
					m_LastGameViewFbW = fbW;
					m_LastGameViewFbH = fbH;
					m_GameViewHasRendered = true;
				}

				// guard's destructor restores the viewport — explicit restore here
				// is no longer needed and would be a redundant double-set.

				ImGui::InvisibleButton("##GameViewCanvas", viewportSize);
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(0, 0, 0, 255));

				// FBO sampling uses default UVs — see RenderEditorView for
				// the rationale. Game View shares the same convention.
				drawList->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(m_GameViewFBO.GetColorTextureBackendId())),
					imageMin,
					imageMax);
				drawList->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 40));

				float statsRenderedHeight = 0.0f;
				if (m_ShowGameViewStats) {
					m_GameViewStatsOverlay.RefreshIfDue(fbW, fbH);
					statsRenderedHeight = m_GameViewStatsOverlay.RenderInRect(imageMin, imageMax);
				}

				if (m_ShowGameViewLogs) {
					if (!m_GameViewLogOverlay) {
						m_GameViewLogOverlay = std::make_unique<Index::Diagnostics::LogOverlay>();
					}
					const float logYOffset = statsRenderedHeight > 0.0f
						? statsRenderedHeight + 8.0f
						: 0.0f;
					m_GameViewLogOverlay->RenderInRect(imageMin, imageMax, logYOffset);
				}
			}
			else {
				drawNoCameraFallback();
			}
		}
		else {
			ImGui::TextDisabled("Game View has no drawable area");
		}

		m_IsGameViewHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
		m_IsGameViewFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		ApplicationEditorAccess::SetGameInputEnabled(m_IsGameViewFocused);
		ImGui::End();
	}

} // namespace Index
