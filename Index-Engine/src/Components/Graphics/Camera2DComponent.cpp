#include "pch.hpp"
#include "Camera2DComponent.hpp"
#include "Collections/Viewport.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace Index {
	Camera2DComponent* Camera2DComponent::Main() {
		Application* app = Application::GetInstance();
		if (!app || !app->GetSceneManager()) {
			return nullptr;
		}

		Scene* activeScene = app->GetSceneManager()->GetActiveScene();
		return activeScene ? activeScene->GetMainCamera() : nullptr;
	}

	Transform2DComponent* Camera2DComponent::TryGetTransform() {
		if (!m_OwnerScene || m_OwnerEntity == entt::null) return nullptr;
		if (!m_OwnerScene->IsValid(m_OwnerEntity)) return nullptr;
		if (!m_OwnerScene->HasComponent<Transform2DComponent>(m_OwnerEntity)) return nullptr;
		return &m_OwnerScene->GetComponent<Transform2DComponent>(m_OwnerEntity);
	}

	const Transform2DComponent* Camera2DComponent::TryGetTransform() const {
		if (!m_OwnerScene || m_OwnerEntity == entt::null) return nullptr;
		if (!m_OwnerScene->IsValid(m_OwnerEntity)) return nullptr;
		if (!m_OwnerScene->HasComponent<Transform2DComponent>(m_OwnerEntity)) return nullptr;
		return &m_OwnerScene->GetComponent<Transform2DComponent>(m_OwnerEntity);
	}

	void Camera2DComponent::UpdateViewport() {
		UpdateProj();
		UpdateView();
	}

	namespace {
		const Transform2DComponent* TryGetParentWorldTransform(Scene* scene, EntityHandle entity) {
			if (!scene || entity == entt::null || !scene->IsValid(entity)) return nullptr;
			if (!scene->HasComponent<HierarchyComponent>(entity)) return nullptr;
			const auto& hierarchy = scene->GetComponent<HierarchyComponent>(entity);
			if (hierarchy.Parent == entt::null || !scene->IsValid(hierarchy.Parent)) return nullptr;
			if (!scene->HasComponent<Transform2DComponent>(hierarchy.Parent)) return nullptr;
			return &scene->GetComponent<Transform2DComponent>(hierarchy.Parent);
		}
	}

	void Camera2DComponent::SetPosition(Vec2 p) {
		if (auto* transform = TryGetTransform()) {
			Vec2 localP = p;
			if (const auto* parentTr = TryGetParentWorldTransform(m_OwnerScene, m_OwnerEntity)) {
				// Inverse of TransformPoint: T^-1 (parent.Position), R^-1
				// (-parent.Rotation), S^-1 (1/parent.Scale). Zero-scale is
				// degenerate; fall through to the rotated value to avoid div-by-zero.
				const Vec2 delta{ p.x - parentTr->Position.x, p.y - parentTr->Position.y };
				const Vec2 unrotated = Rotate(delta, -parentTr->Rotation);
				const float invSx = parentTr->Scale.x != 0.0f ? 1.0f / parentTr->Scale.x : 1.0f;
				const float invSy = parentTr->Scale.y != 0.0f ? 1.0f / parentTr->Scale.y : 1.0f;
				localP = Vec2{ unrotated.x * invSx, unrotated.y * invSy };
			}
			transform->SetPosition(localP);
			// Cache the world value too so UpdateView (and any same-frame
			// reader) sees the new placement before propagation runs.
			transform->Position = p;
			UpdateView();
		}
	}

	void Camera2DComponent::AddPosition(Vec2 p) {
		if (auto* transform = TryGetTransform()) {
			SetPosition(p + transform->Position);
		}
	}

	Vec2 Camera2DComponent::GetPosition() const {
		if (const auto* transform = TryGetTransform()) {
			return transform->Position;
		}
		return Vec2{ 0.0f, 0.0f };
	}

	void Camera2DComponent::SetRotation(float rad) {
		if (auto* transform = TryGetTransform()) {
			float localRad = rad;
			if (const auto* parentTr = TryGetParentWorldTransform(m_OwnerScene, m_OwnerEntity)) {
				localRad = rad - parentTr->Rotation;
			}
			transform->SetRotation(localRad);
			transform->Rotation = rad;
			UpdateView();
		}
	}

	float Camera2DComponent::GetRotation() const {
		if (const auto* transform = TryGetTransform()) {
			return transform->Rotation;
		}
		return 0.0f;
	}

	glm::mat4 Camera2DComponent::GetViewProjectionMatrix() const {
		return m_ProjMat * m_ViewMat;
	}

	void Camera2DComponent::UpdateProj() {
		if (!m_Viewport || m_Viewport->GetWidth() == 0 || m_Viewport->GetHeight() == 0) return;
		const float aspect = m_Viewport->GetAspect();
		const float halfH = m_OrthographicSize * m_Zoom;
		const float halfW = halfH * aspect;

		// Symmetric z range [-1,1] maps world z=0 to NDC z=0, valid under all backends (Vulkan/D3D12 clip [0,1] culls z<0).
		const float zNear = -1.0f;
		const float zFar  =  1.0f;

		m_ProjMat = glm::ortho(-halfW, +halfW, -halfH, +halfH, zNear, zFar);
		if (TryGetTransform()) {
			UpdateViewportAABB();
		}
	}

	void Camera2DComponent::UpdateView() {
		// Closed-form view = Rz(-theta) * T(-p) — replaces per-frame glm::inverse(M).
		const Transform2DComponent* transform = TryGetTransform();
		if (!transform) return;

		const float rotZ = transform->Rotation;
		const float c = std::cos(rotZ);
		const float s = std::sin(rotZ);
		const float px = transform->Position.x;
		const float py = transform->Position.y;

		// Column-major: rotation block is Rz(-theta), translation column is Rz(-theta) * (-p).
		glm::mat4 view(1.0f);
		view[0][0] =  c; view[0][1] = -s; view[0][2] = 0.0f; view[0][3] = 0.0f;
		view[1][0] =  s; view[1][1] =  c; view[1][2] = 0.0f; view[1][3] = 0.0f;
		view[2][0] = 0.0f; view[2][1] = 0.0f; view[2][2] = 1.0f; view[2][3] = 0.0f;
		view[3][0] = -c * px - s * py;
		view[3][1] =  s * px - c * py;
		view[3][2] = 0.0f;
		view[3][3] = 1.0f;
		m_ViewMat = view;

		UpdateViewportAABB();
	}

	void Camera2DComponent::UpdateViewportAABB() {
		if (!m_Viewport) return;

		const Transform2DComponent* transform = TryGetTransform();
		if (!transform) return;
		Vec2 worldViewport = WorldViewPort();
		m_WorldViewportAABB = AABB::Create(transform->Position, worldViewport / 2.f);
	}

	Vec2 Camera2DComponent::WorldViewPort() const {
		if (!m_Viewport) return { 0.0f, 0.0f };

		float aspect = m_Viewport->GetAspect();
		float worldHeight = 2.0f * (m_OrthographicSize * m_Zoom);
		float worldWidth = worldHeight * aspect;
		return { worldWidth, worldHeight };
	}

	Vec2 Camera2DComponent::ScreenToWorld(Vec2 pos) const
	{
		if (!m_Viewport) return { 0.0f, 0.0f };

		// Editor: m_Viewport is restored to OS-window size after RenderGameView's RAII guard, so read panel size from UIRegion. Runtime: UIRegion unset, m_Viewport is the target.
		const Window::UIRegion uiRegion = Window::GetUIRegion();
		float vpW = 0.0f;
		float vpH = 0.0f;
		if (uiRegion.IsActive()) {
			vpW = static_cast<float>(uiRegion.Width);
			vpH = static_cast<float>(uiRegion.Height);
		}
		else {
			vpW = static_cast<float>(m_Viewport->GetWidth());
			vpH = static_cast<float>(m_Viewport->GetHeight());
		}
		if (vpW <= 0.0f || vpH <= 0.0f) return { 0.0f, 0.0f };

		const float xNdc = (2.0f * pos.x / vpW) - 1.0f;
		const float yNdc = 1.0f - (2.0f * pos.y / vpH);

		// Rebuild proj from panel dims rather than cached m_ProjMat: editor restores m_Viewport to OS-window size before scripts run, so aspects may differ.
		const float aspect = vpW / vpH;
		const float halfH = m_OrthographicSize * m_Zoom;
		const float halfW = halfH * aspect;
		const glm::mat4 projMat = glm::ortho(-halfW, +halfW, -halfH, +halfH, -1.0f, 1.0f);

		const glm::vec4 clip(xNdc, yNdc, 0.0f, 1.0f);
		const glm::mat4 vp = projMat * m_ViewMat;
		const glm::mat4 invVp = glm::inverse(vp);

		glm::vec4 world = invVp * clip;
		if (world.w != 0.0f) world /= world.w;

		return { world.x, world.y };
	}

	void Camera2DComponent::Initialize(Scene& scene, EntityHandle entity) {
		m_Viewport = Window::GetMainViewport();
		m_OwnerScene = &scene;
		m_OwnerEntity = entity;
		m_ViewMat = glm::mat4(1.0f);
		m_ProjMat = glm::mat4(1.0f);
		UpdateProj();
		UpdateView();
	}

	void Camera2DComponent::Destroy() {
		m_OwnerScene = nullptr;
		m_OwnerEntity = entt::null;
		m_Viewport = nullptr;
	}
}
