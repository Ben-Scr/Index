#include <pch.hpp>
#include "Systems/ImGuiEditorLayer.hpp"

#include <imgui.h>

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
#include "Graphics/TextureManager.hpp"
#include "Gui/GuiRenderer.hpp"
#include "Gui/EditorIcons.hpp"
#include "Math/Trigonometry.hpp"
#include "Math/VectorMath.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/ComponentInfo.hpp"
#include "Scene/EntityPicker.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Systems/TransformHierarchySystem.hpp"
#include "Systems/UILayoutSystem.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Index {

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

	void ImGuiEditorLayer::DrawEditorComponentGizmos(Scene& scene, bool componentGizmosEnabled) {
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
	
			Gizmo::SetColor(Color(1.0f, 0.65f, 0.10f, 1.0f));
			Gizmo::SetLineWidth(2.0f);
			Gizmo::DrawSquare(transform.Position, transform.Scale, rotationDegrees);
	
			if (scene.HasComponent<Camera2DComponent>(m_SelectedEntity)) {
				auto& camera = scene.GetComponent<Camera2DComponent>(m_SelectedEntity);
				Gizmo::SetColor(Color::White());
				Gizmo::SetLineWidth(1.5f);
				Gizmo::DrawSquare(transform.Position, camera.WorldViewPort(), rotationDegrees);
			}
	
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
				const Vec2 halfExtents = collider.GetHalfExtents();
				const Vec2 worldSize(
					std::abs(halfExtents.x * transform.Scale.x) * 2.0f,
					std::abs(halfExtents.y * transform.Scale.y) * 2.0f);
				Gizmo::SetColor(Color(0.10f, 0.85f, 0.85f, 1.0f));
				Gizmo::SetLineWidth(2.0f);
				Gizmo::DrawSquare(transform.Position, worldSize, rotationDegrees);
			}
	
			if (scene.HasComponent<FastCircleCollider2DComponent>(m_SelectedEntity)) {
				auto& collider = scene.GetComponent<FastCircleCollider2DComponent>(m_SelectedEntity);
				const float worldRadius = collider.GetRadius() * std::max(std::abs(transform.Scale.x), std::abs(transform.Scale.y));
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
						Gizmo::DrawCircle(transform.Position, radius);
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
			// MUST precede gizmo corner reads: UILayoutSystem fires later in RenderEditorView, so force layout now for fresh resolved corners.
			ComputeUILayout(scene);

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
							if (info.drawEditorGizmo && info.has && info.has(selected)) {
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
				DrawEditorComponentGizmos(*renderScene, m_ShowGizmos);

				static const Color k_EditorClearColor(0.18f, 0.18f, 0.20f, 1.0f);
				const Color k_PrefabClearColor(k_PrefabEditClearR, k_PrefabEditClearG, k_PrefabEditClearB, 1.0f);
				const Color& clearColor = IsInPrefabEditMode() ? k_PrefabClearColor : k_EditorClearColor;
				// uiInWorldSpace=true: UI joins sprites and gizmos in
				// the editor camera's world space so the user can pan
				// and zoom around the UI like any scene object.
				RenderSceneIntoFBO(m_EditorViewFBO, *renderScene, vp, viewAABB,
					true, false, clearColor, IsInPrefabEditMode(), true, m_EditorViewDrawMode,
					m_ShowGizmos ? GizmoLayerMask::All : GizmoLayerMask::EditorOnly);
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

				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
					&& ImGui::IsWindowHovered()
					&& !ImGui::IsAnyItemActive())
				{
					const ImVec2 mousePos = ImGui::GetMousePos();
					const float localX = mousePos.x - imageTopLeft.x;
					const float localY = mousePos.y - imageTopLeft.y;
					if (localX >= 0.0f && localX < viewportSize.x
						&& localY >= 0.0f && localY < viewportSize.y)
					{
						// Screen → world via the editor camera's axis-aligned
						// ortho view AABB. Y flips because the FBO's top row
						// is screen-y=0 but world +y points up.
						const AABB camAABB = m_EditorCamera.GetViewportAABB();
						const float u = localX / std::max(1.0f, viewportSize.x);
						const float v = localY / std::max(1.0f, viewportSize.y);
						const Vec2 worldPoint{
							camAABB.Min.x + u * (camAABB.Max.x - camAABB.Min.x),
							camAABB.Max.y - v * (camAABB.Max.y - camAABB.Min.y)
						};

						EntityHandle picked = entt::null;
						EntityPicker::TryPickEntity(*renderScene, worldPoint, picked);

						const bool hasModifier =
							ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
						if (picked != entt::null) {
							if (hasModifier) {
								ToggleEntitySelection(picked, -1);
							}
							else {
								SetSingleEntitySelection(picked, -1);
							}
						}
						else if (!hasModifier) {
							ClearEntitySelection();
						}
					}
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
