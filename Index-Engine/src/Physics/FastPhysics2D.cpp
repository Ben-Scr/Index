#include "pch.hpp"

#include "Physics/FastPhysics2D.hpp"
#include "Physics/PhysicsSystem2D.hpp"
#include "Physics/IndexPhysicsWorld2D.hpp"
#include "Scene/Scene.hpp"
#include "Core/Application.hpp"

#include <Physics2D.hpp>
#include <Collider.hpp>
#include <Body.hpp>

#include <limits>

namespace Index {
	namespace {
		bool IsValidRef(const PhysicsBodyRef2D& ref) {
			return ref.scene && ref.entity != entt::null && ref.scene->IsValid(ref.entity);
		}

		bool EnsurePhysicsQueryThread() {
			if (Application::IsMainThread()) {
				return true;
			}
			IDX_CORE_ASSERT(false, IndexErrorCode::InvalidArgument,
				"FastPhysics2D queries must run on the main thread because they touch the live Index-Physics world.");
			return false;
		}

		// Point IndexPhys::Physics2D at the live Index-Physics world and hand it back.
		// Returns nullptr when physics is uninitialized, disabled, or queried off-thread —
		// callers then surface a miss, matching the Box2D path's behaviour.
		IndexPhysicsWorld2D* AcquireWorld() {
			if (!EnsurePhysicsQueryThread() || !PhysicsSystem2D::IsInitialized() || !PhysicsSystem2D::IsEnabled()) {
				return nullptr;
			}
			IndexPhysicsWorld2D& world = PhysicsSystem2D::GetIndexPhysicsWorld();
			IndexPhys::Physics2D::SetContext(world.GetWorld());
			return &world;
		}

		IndexPhys::Vec2 ToPhys(const Vec2& v) { return IndexPhys::Vec2(v.x, v.y); }

		PhysicsBodyRef2D ResolveRef(const IndexPhysicsWorld2D& world, const IndexPhys::Collider* collider) {
			Scene* scene = nullptr;
			EntityHandle entity = entt::null;
			world.ResolveColliderEntity(collider, scene, entity);
			PhysicsBodyRef2D ref{ entity, scene };
			return IsValidRef(ref) ? ref : PhysicsBodyRef2D{};
		}

		// Resolve every hit collider to a valid (scene, entity); first-valid for First,
		// nearest-by-body-position for Nearest. Mirrors Physics2D's QueryProxyRef.
		std::optional<PhysicsBodyRef2D> PickRef(const IndexPhysicsWorld2D& world,
			const std::vector<const IndexPhys::Collider*>& hits, const Vec2& center, OverlapMode mode) {
			std::optional<PhysicsBodyRef2D> nearest;
			float bestDist2 = std::numeric_limits<float>::max();
			for (const IndexPhys::Collider* collider : hits) {
				PhysicsBodyRef2D ref = ResolveRef(world, collider);
				if (!IsValidRef(ref)) continue;

				if (mode == OverlapMode::First) {
					return ref;
				}

				const IndexPhys::Body* body = collider->GetBody();
				const Vec2 pos = body ? Vec2{ body->GetPosition().x, body->GetPosition().y } : center;
				const float dx = pos.x - center.x;
				const float dy = pos.y - center.y;
				const float d2 = dx * dx + dy * dy;
				if (d2 < bestDist2) {
					bestDist2 = d2;
					nearest = ref;
				}
			}
			return nearest;
		}

		std::vector<PhysicsBodyRef2D> CollectRefs(const IndexPhysicsWorld2D& world,
			const std::vector<const IndexPhys::Collider*>& hits) {
			std::vector<PhysicsBodyRef2D> results;
			results.reserve(hits.size());
			for (const IndexPhys::Collider* collider : hits) {
				PhysicsBodyRef2D ref = ResolveRef(world, collider);
				if (IsValidRef(ref)) {
					results.push_back(ref);
				}
			}
			return results;
		}

		std::vector<IndexPhys::Vec2> ToPhysPolygon(const Vec2& center, const std::vector<Vec2>& points) {
			std::vector<IndexPhys::Vec2> result;
			result.reserve(points.size());
			for (const Vec2& p : points) {
				result.push_back(ToPhys(p));
			}
			(void)center; // points are already relative to origin, as IndexPhys::Physics2D expects.
			return result;
		}
	}

	std::optional<RaycastHit2D> FastPhysics2D::Raycast(const Vec2& origin, const Vec2& direction, float maxDistance) {
		IndexPhysicsWorld2D* world = AcquireWorld();
		if (!world || maxDistance <= 0.0f) {
			return std::nullopt;
		}
		// Reject a zero direction up-front; the library would otherwise normalize to NaN.
		if (direction.x == 0.0f && direction.y == 0.0f) {
			return std::nullopt;
		}

		IndexPhys::RaycastHit hit = IndexPhys::Physics2D::Raycast(ToPhys(origin), ToPhys(direction), maxDistance);
		if (!hit.hit || !hit.collider) {
			return std::nullopt;
		}

		PhysicsBodyRef2D ref = ResolveRef(*world, hit.collider);
		if (!IsValidRef(ref)) {
			return std::nullopt;
		}

		RaycastHit2D out;
		out.entity = ref.entity;
		out.scene = ref.scene;
		out.point = { hit.point.x, hit.point.y };
		out.normal = { hit.normal.x, hit.normal.y };
		out.distance = hit.distance;
		return out;
	}

	std::optional<PhysicsBodyRef2D> FastPhysics2D::OverlapCircleRef(const Vec2& center, float radius, OverlapMode mode) {
		IndexPhysicsWorld2D* world = AcquireWorld();
		if (!world || radius <= 0.0f) {
			return std::nullopt;
		}
		return PickRef(*world, IndexPhys::Physics2D::OverlapCircleAll(ToPhys(center), radius), center, mode);
	}

	std::optional<PhysicsBodyRef2D> FastPhysics2D::OverlapBoxRef(const Vec2& center, const Vec2& halfExtents, float degrees, OverlapMode mode) {
		IndexPhysicsWorld2D* world = AcquireWorld();
		if (!world || halfExtents.x <= 0.0f || halfExtents.y <= 0.0f) {
			return std::nullopt;
		}
		// IndexPhys::Physics2D takes the FULL box size; the script ABI passes half-extents.
		const IndexPhys::Vec2 size(halfExtents.x * 2.0f, halfExtents.y * 2.0f);
		return PickRef(*world, IndexPhys::Physics2D::OverlapBoxAll(ToPhys(center), size, degrees), center, mode);
	}

	std::optional<PhysicsBodyRef2D> FastPhysics2D::OverlapPolygonRef(const Vec2& center, const std::vector<Vec2>& points, OverlapMode mode) {
		IndexPhysicsWorld2D* world = AcquireWorld();
		if (!world || points.size() < 3) {
			return std::nullopt;
		}
		const std::vector<IndexPhys::Vec2> poly = ToPhysPolygon(center, points);
		return PickRef(*world, IndexPhys::Physics2D::OverlapPolygonAll(ToPhys(center), poly.data(), poly.size()), center, mode);
	}

	std::optional<PhysicsBodyRef2D> FastPhysics2D::ContainsPointRef(const Vec2& point, OverlapMode mode) {
		IndexPhysicsWorld2D* world = AcquireWorld();
		if (!world) {
			return std::nullopt;
		}
		return PickRef(*world, IndexPhys::Physics2D::ContainsPointAll(ToPhys(point)), point, mode);
	}

	std::vector<PhysicsBodyRef2D> FastPhysics2D::OverlapCircleAllRefs(const Vec2& center, float radius) {
		IndexPhysicsWorld2D* world = AcquireWorld();
		if (!world || radius <= 0.0f) {
			return {};
		}
		return CollectRefs(*world, IndexPhys::Physics2D::OverlapCircleAll(ToPhys(center), radius));
	}

	std::vector<PhysicsBodyRef2D> FastPhysics2D::OverlapBoxAllRefs(const Vec2& center, const Vec2& halfExtents, float degrees) {
		IndexPhysicsWorld2D* world = AcquireWorld();
		if (!world || halfExtents.x <= 0.0f || halfExtents.y <= 0.0f) {
			return {};
		}
		const IndexPhys::Vec2 size(halfExtents.x * 2.0f, halfExtents.y * 2.0f);
		return CollectRefs(*world, IndexPhys::Physics2D::OverlapBoxAll(ToPhys(center), size, degrees));
	}

	std::vector<PhysicsBodyRef2D> FastPhysics2D::OverlapPolygonAllRefs(const Vec2& center, const std::vector<Vec2>& points) {
		IndexPhysicsWorld2D* world = AcquireWorld();
		if (!world || points.size() < 3) {
			return {};
		}
		const std::vector<IndexPhys::Vec2> poly = ToPhysPolygon(center, points);
		return CollectRefs(*world, IndexPhys::Physics2D::OverlapPolygonAll(ToPhys(center), poly.data(), poly.size()));
	}

	std::vector<PhysicsBodyRef2D> FastPhysics2D::ContainsPointAllRefs(const Vec2& point) {
		IndexPhysicsWorld2D* world = AcquireWorld();
		if (!world) {
			return {};
		}
		return CollectRefs(*world, IndexPhys::Physics2D::ContainsPointAll(ToPhys(point)));
	}
}
