#pragma once
#include <Scene/EntityHandle.hpp>

namespace Axiom {
	class Collider2D;
	class Rigidbody2DComponent;
}

namespace Axiom {
	// Public utility for resolving a physics object back to its owning entity.
	// Only Axiom-typed parameters are exposed: the Box2D-typed overloads
	// (GetEntityHandleFromBodyId / GetEntityHandleFromShapeID) used to live
	// here but were removed because (a) nothing called them outside this
	// translation unit and (b) keeping `b2BodyId` / `b2ShapeId` in a public
	// engine header leaks Box2D into the swappable physics layer. Engine
	// code that has a raw b2 id can call `b2Body_GetUserData(...)` directly
	// — that's the one line they replaced.
	class PhysicsUtility {
	public:
		static EntityHandle GetEntityHandleFromCollider(const Collider2D& collider);
		static EntityHandle GetEntityHandleFromRigidbody(const Rigidbody2DComponent& rb);
	};
}
