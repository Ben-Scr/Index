#pragma once
#include <Scene/EntityHandle.hpp>

namespace Index {
	class Collider2D;
	class Rigidbody2DComponent;
}

namespace Index {
	// b2BodyId/b2ShapeId overloads intentionally removed to avoid leaking Box2D types into public headers; callers with raw b2 ids use b2Body_GetUserData() directly.
	class PhysicsUtility {
	public:
		static EntityHandle GetEntityHandleFromCollider(const Collider2D& collider);
		static EntityHandle GetEntityHandleFromRigidbody(const Rigidbody2DComponent& rb);
	};
}
