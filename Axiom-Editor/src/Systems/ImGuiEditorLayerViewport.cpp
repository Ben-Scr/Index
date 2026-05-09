#include <pch.hpp>
#include "Systems/ImGuiEditorLayer.hpp"

#include <imgui.h>
#include <glad/glad.h>

#include "Components/Components.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Diagnostics/StatsOverlay.hpp"
#include "Editor/ApplicationEditorAccess.hpp"
#include "Graphics/GizmoRenderer.hpp"
#include "Graphics/Gizmo.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/GuiRenderer.hpp"
#include "Gui/EditorIcons.hpp"
#include "Math/Trigonometry.hpp"
#include "Math/VectorMath.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/ComponentInfo.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Systems/TransformHierarchySystem.hpp"
#include "Systems/UILayoutSystem.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace Axiom {
	namespace {
		struct GameViewAspectPreset {
			const char* Label;
			float Aspect = 0.0f;
		};

		constexpr std::array<GameViewAspectPreset, 6> k_GameViewAspectPresets = {{
			{ "Free Aspect", 0.0f },
			{ "16:9", 16.0f / 9.0f },
			{ "21:9", 21.0f / 9.0f },
			{ "4:3", 4.0f / 3.0f },
			{ "3:2", 3.0f / 2.0f },
			{ "9:16", 9.0f / 16.0f }
		}};
	}

	void ImGuiEditorLayer::RenderSceneIntoFBO(ViewportFBO& fbo, Scene& scene,
		const glm::mat4& vp, const AABB& viewportAABB,
		bool withGizmos, bool sharedGizmosOnly, const Color& clearColor,
		bool onlyPassedScene, bool uiInWorldSpace,
		EditorViewDrawMode drawMode)
	{
		auto* app = Application::GetInstance();
		if (!app) return;
		auto* renderer = app->GetRenderer2D();
		if (!renderer) return;

		int w = fbo.ViewportSize.GetWidth();
		int h = fbo.ViewportSize.GetHeight();

		glBindFramebuffer(GL_FRAMEBUFFER, fbo.FramebufferId);
		glViewport(0, 0, w, h);
		glClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		// Sprites + UI + (optional) gizmos extracted into a callable so
		// the Mixed draw mode can run the scene twice — once filled and
		// once in wireframe — into the same FBO without duplicating the
		// surrounding setup. Triangle mode flips polygon mode to GL_LINE
		// for a single pass; Default keeps the historical behaviour.
		auto runSceneRender = [&]() {
			if (onlyPassedScene) {
				renderer->RenderSceneWithVP(scene, vp, viewportAABB);
			}
			else {
				SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
					renderer->RenderSceneWithVP(s, vp, viewportAABB);
					});
			}

			// UI must paint into this panel's FBO (not the OS window
			// backbuffer that ImGui will cover). The auto BeginFrame path
			// is skipped in editor mode, so we drive RenderScene here while
			// the FBO is still bound.
			//
			// World-space mode (Editor View) projects through the same `vp`
			// the world used so UI rects translate/scale with the camera —
			// matching how the selection gizmo for a RectTransform2D draws.
			// Screen-space mode (Game View, runtime) keeps UI locked to the
			// canvas regardless of camera, the way runtime UI behaves.
			//
			// UI submits BEFORE gizmos so the gizmo pass paints on top —
			// selection outlines, manipulators, and debug overlays must be
			// visible even when the user is editing UI that fills the viewport.
			if (auto* gui = app->GetGuiRenderer()) {
				// Resolve the canonical pixel-to-world scale once per pass and
				// thread it through to RenderScene so the renderer doesn't reach
				// into Application / main-camera state itself.
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
				const GizmoLayerMask layerMask = sharedGizmosOnly ? GizmoLayerMask::Shared : GizmoLayerMask::All;
				GizmoRenderer2D::RenderWithVP(vp, layerMask);
			}
		};

		// Wireframe pass uses glColorLogicOp(GL_CLEAR) instead of relying
		// on each shader to honour a debug uniform. GL_CLEAR forces every
		// touched pixel's RGB to 0 *after* the fragment shader runs, and
		// the alpha mask keeps the FBO opaque. The net effect is "every
		// triangle edge becomes solid black, regardless of what the
		// entity's shader/texture/colour would normally output." Logic op
		// also overrides blending, so semi-transparent quads still emit
		// an opaque black edge — which is what the user wants in this
		// debug view.
		auto runWireframePass = [&]() {
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			glEnable(GL_COLOR_LOGIC_OP);
			glLogicOp(GL_CLEAR);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
			runSceneRender();
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glLogicOp(GL_COPY);
			glDisable(GL_COLOR_LOGIC_OP);
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		};

		switch (drawMode) {
		case EditorViewDrawMode::Triangle:
			runWireframePass();
			break;
		case EditorViewDrawMode::Mixed:
			runSceneRender();
			runWireframePass();
			break;
		case EditorViewDrawMode::Default:
		default:
			runSceneRender();
			break;
		}

		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		auto* window = Application::GetWindow();
		if (window) {
			glViewport(0, 0, window->GetWidth(), window->GetHeight());
		}
	}

	void ImGuiEditorLayer::DrawEditorComponentGizmos(Scene& scene) {
		if (m_SelectedEntity == entt::null || !scene.IsValid(m_SelectedEntity)) {
			return;
		}

		const bool hasTransform = scene.HasComponent<Transform2DComponent>(m_SelectedEntity);
		const bool hasRectTransform = scene.HasComponent<RectTransform2DComponent>(m_SelectedEntity);

		// Per-component gizmos (registered by packages) are dispatched below
		// regardless of transform presence, so we no longer early-out when a
		// selection has neither Transform2D nor RectTransform2D — a registered
		// callback may want to paint on transformless entities (rare, but the
		// package surface allows it). Save/restore of gizmo state is cheap.

		const Color previousColor = Gizmo::GetColor();
		const float previousLineWidth = Gizmo::GetLineWidth();
		const GizmoLayer previousLayer = Gizmo::GetLayer();

		Gizmo::SetLayer(GizmoLayer::EditorOnly);

		if (hasTransform) {
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
					// Outline by drawing line segments between consecutive world-space
					// vertices — Box2D's GetPolygon already applies the offset, scale,
					// and any custom hull, so we just need to project through the
					// entity's rotation to get the on-screen position.
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
			// UILayoutSystem normally runs inside GuiRenderer::RenderScene,
			// which fires later in this same RenderEditorView pass. Force a
			// layout refresh now so we read fresh resolved screen-space corners
			// for the selected rect (and its ancestors).
			ComputeUILayout(scene);

			auto& rect = scene.GetComponent<RectTransform2DComponent>(m_SelectedEntity);

			// The Editor View renders UI in world space (see
			// RenderSceneIntoFBO with uiInWorldSpace=true), scaling the
			// resolved UI-pixel coords by ComputeWorldUIPixelScale().
			// Mirror that scale here so the outline lands exactly on
			// the rendered widget instead of drifting away with camera
			// pan/zoom.
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
		}

		// Package-registered viewport gizmos. Walk every registered component
		// type and invoke `drawEditorGizmo` on any one the selected entity has.
		// Built-in components don't set this today (they paint via the
		// hardcoded branches above); the hook exists so packages — Tilemap2D,
		// future tools — can paint their own selection helper without the
		// editor having to learn about package-side types.
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

		Gizmo::SetLayer(previousLayer);
		Gizmo::SetColor(previousColor);
		Gizmo::SetLineWidth(previousLineWidth);
	}

	void ImGuiEditorLayer::RenderEditorView(Scene& scene) {
		m_IsEditorViewActive = ImGui::Begin("Editor View");

		if (!m_IsEditorViewActive) {
			m_IsEditorViewHovered = false;
			m_IsEditorViewFocused = false;
			ImGui::End();
			return;
		}

		// ── Top toolbar ─────────────────────────────────────────────
		// Mirror the Game View's per-window options strip: a single
		// row pinned above the viewport image. Currently exposes the
		// draw-mode selector (Default / Triangle / Mixed); add new
		// editor-only viewport options here so they group naturally.
		{
			constexpr const char* k_DrawModeLabels[] = { "Default", "Triangle", "Mixed" };
			const int currentIndex = static_cast<int>(m_EditorViewDrawMode);
			ImGui::TextUnformatted("Draw:");
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
		}

		// Inspector edits and other panels run earlier in this same OnPreRender
		// phase — AFTER the scene system's OnPreRender propagation already
		// fired — so re-propagate here before either the gizmo pass or the
		// FBO render samples Position/Scale/Rotation. Without this, an edit
		// to a Local* field would only show up one frame later.
		if (IsInPrefabEditMode()) {
			TransformHierarchySystem::Propagate(*m_PrefabEditScene);
		}
		else {
			SceneManager::Get().ForeachLoadedScene([](Scene& s) {
				TransformHierarchySystem::Propagate(s);
				});
		}

		// In prefab-edit mode, every reference to the inspector's "scene"
		// in this function points at the detached prefab scene. We don't
		// want gizmos / camera icons / framebuffer renders to leak the
		// active scene's contents into the prefab viewport.
		Scene* renderScene = IsInPrefabEditMode() ? m_PrefabEditScene.get() : &scene;

		const ImVec2 viewportSize = ImGui::GetContentRegionAvail();
		const int fbW = static_cast<int>(viewportSize.x);
		const int fbH = static_cast<int>(viewportSize.y);

		if (fbW > 0 && fbH > 0) {

			EnsureFBO(m_EditorViewFBO, fbW, fbH);
			m_EditorCamera.SetViewportSize(fbW, fbH);

			if (m_EditorViewFBO.FramebufferId != 0) {

				auto* app = Application::GetInstance();
				if (app) {
					auto& input = app->GetInput();
					float dt = app->GetTime().GetDeltaTimeUnscaled();

					Vec2 mouseDelta = { 0.0f, 0.0f };
					if (m_IsEditorViewHovered && input.GetMouse(MouseButton::Middle)) {
						mouseDelta = input.GetMouseDelta();
					}
					float scroll = m_IsEditorViewHovered ? input.ScrollValue() : 0.0f;

					m_EditorCamera.Update(dt, m_IsEditorViewHovered, mouseDelta, scroll);
				}

				glm::mat4 vp = m_EditorCamera.GetViewProjectionMatrix();
				AABB viewAABB = m_EditorCamera.GetViewportAABB();
				Gizmo::SetViewportAABBOverride(viewAABB);
				DrawEditorComponentGizmos(*renderScene);

				static const Color k_EditorClearColor(0.18f, 0.18f, 0.20f, 1.0f);
				const Color k_PrefabClearColor(k_PrefabEditClearR, k_PrefabEditClearG, k_PrefabEditClearB, 1.0f);
				const Color& clearColor = IsInPrefabEditMode() ? k_PrefabClearColor : k_EditorClearColor;
				// uiInWorldSpace=true: UI joins sprites and gizmos in
				// the editor camera's world space so the user can pan
				// and zoom around the UI like any scene object.
				RenderSceneIntoFBO(m_EditorViewFBO, *renderScene, vp, viewAABB, true, false, clearColor, IsInPrefabEditMode(), true, m_EditorViewDrawMode);
				Gizmo::ClearViewportAABBOverride();

				ImGui::Image(
					static_cast<ImTextureID>(static_cast<intptr_t>(m_EditorViewFBO.ColorTextureId)),
					viewportSize,
					ImVec2(0.0f, 1.0f),
					ImVec2(1.0f, 0.0f));

				ImVec2 imageTopLeft = ImGui::GetItemRectMin();

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

					unsigned int camIcon = EditorIcons::Get("camera", 24);
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

		// Same rationale as RenderEditorView — propagate any inspector edits
		// from earlier in this OnPreRender phase before reading transforms.
		SceneManager::Get().ForeachLoadedScene([](Scene& s) {
			TransformHierarchySystem::Propagate(s);
			});

		const int aspectPresetIndex = std::clamp(m_GameViewAspectPresetIndex, 0, static_cast<int>(k_GameViewAspectPresets.size()) - 1);
		m_GameViewAspectPresetIndex = aspectPresetIndex;
		if (!m_GameViewAspectLoaded) {
			if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
				for (int i = 0; i < static_cast<int>(k_GameViewAspectPresets.size()); ++i) {
					if (project->GameViewAspect == k_GameViewAspectPresets[i].Label) {
						m_GameViewAspectPresetIndex = i;
						break;
					}
				}
			}
			m_GameViewAspectLoaded = true;
		}
		if (!m_GameViewVsyncLoaded) {
			if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
				m_GameViewVsync = project->GameViewVsync;
			}
			m_GameViewVsyncLoaded = true;
		}

		ImGui::SetNextItemWidth(140.0f);
		if (ImGui::BeginCombo("##GameViewAspect", k_GameViewAspectPresets[m_GameViewAspectPresetIndex].Label)) {
			for (int i = 0; i < static_cast<int>(k_GameViewAspectPresets.size()); ++i) {
				const bool selected = (i == m_GameViewAspectPresetIndex);
				if (ImGui::Selectable(k_GameViewAspectPresets[i].Label, selected)) {
					m_GameViewAspectPresetIndex = i;
					if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
						project->GameViewAspect = k_GameViewAspectPresets[i].Label;
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
			if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
				project->GameViewVsync = m_GameViewVsync;
				project->Save();
			}
		}

		// "Stats" toggle. Lives next to VSync; flips m_ShowGameViewStats so
		// the overlay block at the bottom of this function decides whether
		// to draw. Buttons show a depressed appearance when their overlay
		// is active so the user can see at a glance which are on.
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
		const float targetAspect = k_GameViewAspectPresets[m_GameViewAspectPresetIndex].Aspect;

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
			EnsureFBO(m_GameViewFBO, fbW, fbH);

			Camera2DComponent* gameCam = Camera2DComponent::Main();
			if (m_GameViewFBO.FramebufferId != 0 && gameCam && gameCam->IsValid()) {
				Viewport* savedViewport = gameCam->GetViewport();
				if (!savedViewport) {
					ImGui::TextDisabled("Main camera has no viewport");
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
				if (m_GameViewVsync) {
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
				const ImVec2 canvasMin = ImGui::GetItemRectMin();
				const ImVec2 canvasMax = ImGui::GetItemRectMax();
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(0, 0, 0, 255));

				const ImVec2 imageMin(
					canvasMin.x + (viewportSize.x - renderSize.x) * 0.5f,
					canvasMin.y + (viewportSize.y - renderSize.y) * 0.5f);
				const ImVec2 imageMax(imageMin.x + renderSize.x, imageMin.y + renderSize.y);

				// Publish the panel's pixel rect so engine UI systems
				// (UILayoutSystem, UIEventSystem, GuiRenderer) resolve in
				// panel-relative coordinates instead of full-OS-window
				// coordinates. Without this, mouse hit-tests miss widgets
				// because the rendered position (panel-centered) differs
				// from the layout-resolved position (window-centered).
				Window::SetUIRegion(
					static_cast<int>(imageMin.x),
					static_cast<int>(imageMin.y),
					static_cast<int>(renderSize.x),
					static_cast<int>(renderSize.y));

				drawList->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(m_GameViewFBO.ColorTextureId)),
					imageMin,
					imageMax,
					ImVec2(0.0f, 1.0f),
					ImVec2(1.0f, 0.0f));
				drawList->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 40));

				// Stats overlay — engine-level helper. The cached snapshot
				// refreshes at 30 Hz internally; we just feed the FBO size
				// and let it draw inside the rendered image rectangle.
				// Tracks rendered height so the log overlay below can stack.
				float statsRenderedHeight = 0.0f;
				if (m_ShowGameViewStats) {
					m_GameViewStatsOverlay.RefreshIfDue(fbW, fbH);
					statsRenderedHeight = m_GameViewStatsOverlay.RenderInRect(imageMin, imageMax);
				}

				// Log overlay — pinned same place as stats; offset down when
				// stats is also visible so the two don't overlap. Lazy-
				// constructed: subscribing to Log::OnLog at engine load is
				// safe (Log is initialized very early), but we mirror the
				// runtime's lazy-construction style for symmetry.
				if (m_ShowGameViewLogs) {
					if (!m_GameViewLogOverlay) {
						m_GameViewLogOverlay = std::make_unique<Axiom::Diagnostics::LogOverlay>();
					}
					const float logYOffset = statsRenderedHeight > 0.0f
						? statsRenderedHeight + 8.0f
						: 0.0f;
					m_GameViewLogOverlay->RenderInRect(imageMin, imageMax, logYOffset);
				}
			}
			else {
				ImGui::TextDisabled("No main camera in scene");
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

} // namespace Axiom
