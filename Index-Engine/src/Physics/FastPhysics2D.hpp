#pragma once
#include "Collections/Vec2.hpp"
#include "Physics/OverlapMode.hpp"
#include "Physics/Physics2D.hpp"
#include "Physics/RaycastHit2D.hpp"
#include "Scene/EntityHandle.hpp"

#include <optional>
#include <vector>

namespace Index {

	// Scene-aware query API for the lightweight Index-Physics world — the world that
	// backs FastBody2D / FastBoxCollider2D / FastCircleCollider2D. Mirrors Physics2D
	// (Box2D), but runs against IndexPhys::Physics2D so script-side FastPhysics2D
	// resolves to Index-Physics colliders rather than Box2D shapes. Results carry the
	// owning Scene + entity via the shared PhysicsBodyRef2D.
	class FastPhysics2D {
	public:
		static std::optional<RaycastHit2D> Raycast(const Vec2& origin, const Vec2& direction, float maxDistance);

		static std::optional<PhysicsBodyRef2D> OverlapCircleRef(const Vec2& center, float radius, OverlapMode mode);
		static std::optional<PhysicsBodyRef2D> OverlapBoxRef(const Vec2& center, const Vec2& halfExtents, float degrees, OverlapMode mode);
		static std::optional<PhysicsBodyRef2D> OverlapPolygonRef(const Vec2& center, const std::vector<Vec2>& points, OverlapMode mode);
		static std::optional<PhysicsBodyRef2D> ContainsPointRef(const Vec2& point, OverlapMode mode);

		static std::vector<PhysicsBodyRef2D> OverlapCircleAllRefs(const Vec2& center, float radius);
		static std::vector<PhysicsBodyRef2D> OverlapBoxAllRefs(const Vec2& center, const Vec2& halfExtents, float degrees);
		static std::vector<PhysicsBodyRef2D> OverlapPolygonAllRefs(const Vec2& center, const std::vector<Vec2>& points);
		static std::vector<PhysicsBodyRef2D> ContainsPointAllRefs(const Vec2& point);
	};
}
