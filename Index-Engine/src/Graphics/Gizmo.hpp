#pragma once
#include "Collections/AABB.hpp"
#include "Collections/Color.hpp"
#include <Math/Math.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Index {
	enum class GizmoLayer : uint8_t {
		EditorOnly = 1 << 0,
		Shared = 1 << 1
	};

	enum class GizmoLayerMask : uint8_t {
		None = 0,
		EditorOnly = static_cast<uint8_t>(GizmoLayer::EditorOnly),
		Shared = static_cast<uint8_t>(GizmoLayer::Shared),
		All = static_cast<uint8_t>(GizmoLayer::EditorOnly) | static_cast<uint8_t>(GizmoLayer::Shared)
	};

	constexpr GizmoLayerMask operator|(GizmoLayerMask lhs, GizmoLayerMask rhs) {
		return static_cast<GizmoLayerMask>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
	}

	constexpr bool HasAnyLayer(GizmoLayer layer, GizmoLayerMask mask) {
		return (static_cast<uint8_t>(layer) & static_cast<uint8_t>(mask)) != 0;
	}

	struct INDEX_API Square { Vec2 Center, HalfExtents; float Radiant; Color Color; GizmoLayer Layer = GizmoLayer::Shared; };
	struct INDEX_API Circle { Vec2 Center; float Radius; int Segments; Color Color; GizmoLayer Layer = GizmoLayer::Shared; };
	struct INDEX_API Line { Vec2 Start; Vec2 End; Color Color; GizmoLayer Layer = GizmoLayer::Shared; };
	// Width-aware line emitted as triangle geometry — WebGPU's LineList topology
	// ignores line width, so DrawLine is always 1px. HalfWidth is in world units.
	struct INDEX_API ThickLine { Vec2 Start; Vec2 End; float HalfWidth; Color Color; GizmoLayer Layer = GizmoLayer::Shared; };

	// Engine-internal (never crosses the DLL boundary, so no INDEX_API — would
	// only earn a C4251 for the std::string member).
	struct GizmoText { Vec2 Position; float Radiant; float Size; std::string Text; Color Color; GizmoLayer Layer = GizmoLayer::Shared; };

	struct INDEX_API Box { Vec3 Center, HalfExtents; float Radiant; Color Color; };
	struct INDEX_API Sphere { Vec3 Center; float Radius; int Segments; Color Color; };
	struct INDEX_API Line3D { Vec3 Start; Vec3 End; Color Color; };

	class INDEX_API Gizmo {
	public:
		// 2D
		static void DrawLine(const Vec2& start, const Vec2& end);
		// Width-aware line (width in world units) emitted as triangles, so it
		// honours thickness unlike DrawLine. Renders in the filled gizmo pass.
		static void DrawThickLine(const Vec2& start, const Vec2& end, float width);

		// Wireframe (outline only).
		static void DrawWireSquare(const Vec2& center, const Vec2& scale, float degrees = 0.0f);
		static void DrawWireCircle(const Vec2& center, float radius, int segments = 32);

		// Filled (solid).
		static void DrawSquare(const Vec2& center, const Vec2& scale, float degrees = 0.0f);
		static void DrawCircle(const Vec2& center, float radius, int segments = 32);

		// World-space text. rotation is in degrees; size is in pixels (matches TextRendererComponent::FontSize).
		static void DrawText(const std::string& text, const Vec2& position, float rotation = 0.0f, float size = 11.0f);

		// 3D
		/*
		static void DrawBox(const Vec3& center, const Vec3& scale, const Vec3& rotation);
		static void DrawSphere(const Vec3& center, const float radius);
		static void DrawLine3D(const Vec3& start, const Vec3& end);
		*/

		static void SetLineWidth(float width) { s_LineWidth = Max(0.001f, width); }
		static float GetLineWidth() { return s_LineWidth; }

		static void SetEnabled(bool enabled) { s_IsEnabled = enabled; }
		static bool IsEnabled() { return s_IsEnabled; }

		static void SetShowInRuntime(bool show) { s_ShowInRuntime = show; }
		static bool GetShowInRuntime() { return s_ShowInRuntime; }

		static void SetColor(const Color& color) { s_Color = color; }
		static Color GetColor() { return s_Color; }
		static void SetLayer(GizmoLayer layer) { s_Layer = layer; }
		static GizmoLayer GetLayer() { return s_Layer; }

		static void SetMaxVertices(size_t maxVertices) { s_MaxVertices = maxVertices; };
		static size_t GetMaxVertices() { return s_MaxVertices; }

		static size_t GetRegisteredVertices() { return s_RegisteredVertices; }
		static void SetViewportAABBOverride(const AABB& viewportAABB) { s_CamViewportAABB = viewportAABB; s_HasViewportOverride = true; }
		static void ClearViewportAABBOverride() { s_HasViewportOverride = false; }
		static bool HasViewportAABBOverride() { return s_HasViewportOverride; }
		static const AABB& GetViewportAABBOverride() { return s_CamViewportAABB; }

		static void Clear();
		static AABB s_CamViewportAABB;
	private:
		static bool s_HasViewportOverride;
		static bool s_IsEnabled;
		static bool s_ShowInRuntime;
		static GizmoLayer s_Layer;

		static  Color s_Color;;

		static const size_t k_BoxVertices = 4;
		static const size_t k_LineVertices = 1;
		static const size_t k_ThickLineVertices = 6; // 2 triangles per thick segment
		static float s_LineWidth;

		static std::vector<Square> s_Squares;
		static std::vector<Circle> s_Circles;
		static std::vector<Line> s_Lines;
		static std::vector<ThickLine> s_ThickLines;
		static std::vector<Square> s_FilledSquares;
		static std::vector<Circle> s_FilledCircles;
		static std::vector<GizmoText> s_Texts;

		static size_t s_MaxVertices;
		static size_t s_RegisteredVertices;

		friend class GizmoRenderer2D;
		friend class Renderer2D;
	};
}
