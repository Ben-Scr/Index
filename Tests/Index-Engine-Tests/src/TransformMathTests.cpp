#include <doctest/doctest.h>

#include "Collections/Mat2.hpp"
#include "Components/General/Transform2DComponent.hpp"

#include <glm/glm.hpp>

using namespace Index;

TEST_CASE("Transform rotation helpers share the same handedness")
{
	const float radians = Radians(35.0f);
	const Vec2 localPoint{ -5.0f, 0.0f };

	Transform2DComponent parent;
	parent.Position = { 0.0f, 0.0f };
	parent.Scale = { 1.0f, 1.0f };
	parent.Rotation = radians;

	const Vec2 transformed = parent.TransformPoint(localPoint);
	const Vec2 rotated = Rotation(radians) * localPoint;

	const float expectedX = -5.0f * Cos(radians);
	const float expectedY = -5.0f * Sin(radians);

	CHECK(transformed.x == doctest::Approx(expectedX));
	CHECK(transformed.y == doctest::Approx(expectedY));
	CHECK(rotated.x == doctest::Approx(transformed.x));
	CHECK(rotated.y == doctest::Approx(transformed.y));
}

TEST_CASE("TransformPoint composes scale, then rotation, then translation")
{
	Transform2DComponent t;
	t.Position = { 10.0f, -4.0f };
	t.Scale = { 2.0f, 3.0f };
	t.Rotation = Radians(90.0f);

	// local (1,0) --S--> (2,0) --R90--> (0,2) --T--> (10,-2)
	const Vec2 p = t.TransformPoint({ 1.0f, 0.0f });
	CHECK(p.x == doctest::Approx(10.0f));
	CHECK(p.y == doctest::Approx(-2.0f));
}

TEST_CASE("TransformVector applies scale and rotation but not translation")
{
	Transform2DComponent t;
	t.Position = { 100.0f, 50.0f };
	t.Scale = { 2.0f, 2.0f };
	t.Rotation = Radians(30.0f);

	const Vec2 local{ 3.0f, -1.0f };
	const Vec2 asVector = t.TransformVector(local);
	const Vec2 asPoint = t.TransformPoint(local);

	// The only difference between the two is the translation, which equals the
	// image of the origin (= Position).
	CHECK(asPoint.x - asVector.x == doctest::Approx(t.Position.x));
	CHECK(asPoint.y - asVector.y == doctest::Approx(t.Position.y));
}

TEST_CASE("Identity transform leaves points and vectors unchanged")
{
	Transform2DComponent t; // defaults: pos 0, scale 1, rotation 0
	const Vec2 v{ 7.0f, -2.5f };

	const Vec2 p = t.TransformPoint(v);
	const Vec2 d = t.TransformVector(v);
	CHECK(p.x == doctest::Approx(v.x));
	CHECK(p.y == doctest::Approx(v.y));
	CHECK(d.x == doctest::Approx(v.x));
	CHECK(d.y == doctest::Approx(v.y));
}

TEST_CASE("GetModelMatrix agrees with TransformPoint")
{
	// Guards against the two code paths drifting apart.
	Transform2DComponent t;
	t.Position = { -3.0f, 8.0f };
	t.Scale = { 1.5f, 0.5f };
	t.Rotation = Radians(40.0f);

	const Vec2 local{ 2.0f, -4.0f };
	const Vec2 viaMethod = t.TransformPoint(local);
	const glm::vec3 viaMatrix = t.GetModelMatrix() * glm::vec3(local.x, local.y, 1.0f);

	CHECK(viaMatrix.x == doctest::Approx(viaMethod.x));
	CHECK(viaMatrix.y == doctest::Approx(viaMethod.y));
}

TEST_CASE("FromPosition and FromScale set world + local fields and leave the rest default")
{
	const Transform2DComponent p = Transform2DComponent::FromPosition({ 5.0f, 6.0f });
	CHECK(p.Position.x == doctest::Approx(5.0f));
	CHECK(p.Position.y == doctest::Approx(6.0f));
	CHECK(p.LocalPosition.x == doctest::Approx(5.0f));
	CHECK(p.Scale.x == doctest::Approx(1.0f));   // untouched default
	CHECK(p.Rotation == doctest::Approx(0.0f));

	const Transform2DComponent s = Transform2DComponent::FromScale({ 2.0f, 4.0f });
	CHECK(s.Scale.x == doctest::Approx(2.0f));
	CHECK(s.Scale.y == doctest::Approx(4.0f));
	CHECK(s.LocalScale.y == doctest::Approx(4.0f));
	CHECK(s.Position.x == doctest::Approx(0.0f)); // untouched default
}

TEST_CASE("GetRotationDegrees round-trips radians")
{
	Transform2DComponent t;
	t.Rotation = Radians(135.0f);
	CHECK(t.GetRotationDegrees() == doctest::Approx(135.0f));
}

TEST_CASE("GetForwardDirection points along +Y at zero rotation")
{
	Transform2DComponent t;
	CHECK(t.GetForwardDirection().x == doctest::Approx(0.0f));
	CHECK(t.GetForwardDirection().y == doctest::Approx(1.0f));

	t.Rotation = Radians(90.0f);
	CHECK(t.GetForwardDirection().x == doctest::Approx(1.0f));
	CHECK(t.GetForwardDirection().y == doctest::Approx(0.0f));
}

TEST_CASE("Transform equality compares world TRS and ignores local/dirty state")
{
	Transform2DComponent a;
	a.Position = { 1.0f, 2.0f };
	a.Rotation = 0.5f;
	a.Scale = { 2.0f, 2.0f };

	Transform2DComponent b = a;
	b.LocalPosition = { 99.0f, 99.0f }; // Local* and the dirty flag are not part
	b.LocalRotation = 9.0f;             // of equality...
	b.ClearDirty();
	CHECK(a == b);

	b.Position.x += 1.0f;               // ...but world Position is.
	CHECK(a != b);
}
