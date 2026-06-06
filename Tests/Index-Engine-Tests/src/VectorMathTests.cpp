#include <doctest/doctest.h>

#include "Math/VectorMath.hpp"

#include <glm/glm.hpp>

using namespace Index;

TEST_CASE("Dot, Length and LengthSquared agree with hand-computed values")
{
	const Vec2 a{ 3.0f, 4.0f };
	const Vec2 b{ -1.0f, 2.0f };

	CHECK(Dot(a, b) == doctest::Approx(3.0f * -1.0f + 4.0f * 2.0f)); // 5
	CHECK(LengthSquared(a) == doctest::Approx(25.0f));
	CHECK(Length(a) == doctest::Approx(5.0f));

	// LengthSquared is Dot(a, a) and Length is its square root.
	CHECK(LengthSquared(a) == doctest::Approx(Dot(a, a)));
	CHECK(Length(b) == doctest::Approx(std::sqrt(LengthSquared(b))));
}

TEST_CASE("Normalized returns a unit vector preserving direction")
{
	const Vec2 v{ 0.0f, 8.0f };
	const Vec2 n = Normalized(v);

	CHECK(n.x == doctest::Approx(0.0f));
	CHECK(n.y == doctest::Approx(1.0f));
	CHECK(Length(n) == doctest::Approx(1.0f));

	const Vec2 m = Normalized(Vec2{ 3.0f, 4.0f });
	CHECK(Length(m) == doctest::Approx(1.0f));
	CHECK(m.x == doctest::Approx(0.6f));
	CHECK(m.y == doctest::Approx(0.8f));
}

TEST_CASE("Normalized guards against the zero vector and returns zero")
{
	const Vec2 n = Normalized(Vec2{ 0.0f, 0.0f });
	CHECK(n.x == doctest::Approx(0.0f));
	CHECK(n.y == doctest::Approx(0.0f));
}

TEST_CASE("Distance and DistanceSquared measure separation between points")
{
	const Vec2 a{ 1.0f, 2.0f };
	const Vec2 b{ 4.0f, 6.0f };

	CHECK(DistanceSquared(a, b) == doctest::Approx(25.0f)); // 3^2 + 4^2
	CHECK(Distance(a, b) == doctest::Approx(5.0f));

	// Symmetric, and zero for coincident points.
	CHECK(Distance(a, b) == doctest::Approx(Distance(b, a)));
	CHECK(Distance(a, a) == doctest::Approx(0.0f));

	// Distance equals the length of the difference vector.
	CHECK(Distance(a, b) == doctest::Approx(Length(a - b)));
}

TEST_CASE("Lerp interpolates linearly between endpoints")
{
	const Vec2 a{ 0.0f, 0.0f };
	const Vec2 b{ 10.0f, -4.0f };

	const Vec2 start = Lerp(a, b, 0.0f);
	CHECK(start.x == doctest::Approx(a.x));
	CHECK(start.y == doctest::Approx(a.y));

	const Vec2 end = Lerp(a, b, 1.0f);
	CHECK(end.x == doctest::Approx(b.x));
	CHECK(end.y == doctest::Approx(b.y));

	const Vec2 mid = Lerp(a, b, 0.5f);
	CHECK(mid.x == doctest::Approx(5.0f));
	CHECK(mid.y == doctest::Approx(-2.0f));
}

TEST_CASE("Rotated turns a vector while preserving its length")
{
	const Vec2 v{ 1.0f, 0.0f };
	const Vec2 r = Rotated(v, Radians(90.0f));

	CHECK(r.x == doctest::Approx(0.0f));
	CHECK(r.y == doctest::Approx(1.0f));
	CHECK(Length(r) == doctest::Approx(Length(v)));

	// A zero rotation leaves the vector unchanged.
	const Vec2 same = Rotated(v, 0.0f);
	CHECK(same.x == doctest::Approx(v.x));
	CHECK(same.y == doctest::Approx(v.y));
}

TEST_CASE("Angle and FromAngle are inverse operations on the unit circle")
{
	// FromAngle builds a unit vector at the requested angle...
	const float theta = Radians(30.0f);
	const Vec2 dir = FromAngle(theta);
	CHECK(dir.x == doctest::Approx(std::cos(theta)));
	CHECK(dir.y == doctest::Approx(std::sin(theta)));
	CHECK(Length(dir) == doctest::Approx(1.0f));

	// ...and Angle recovers that angle.
	CHECK(Angle(dir) == doctest::Approx(theta));
	CHECK(Angle(Vec2{ 1.0f, 0.0f }) == doctest::Approx(0.0f));
	CHECK(Angle(Vec2{ 0.0f, 1.0f }) == doctest::Approx(Radians(90.0f)));
}

TEST_CASE("Named constant directions return the expected unit axes")
{
	CHECK(Zero().x == doctest::Approx(0.0f));
	CHECK(Zero().y == doctest::Approx(0.0f));

	CHECK(One().x == doctest::Approx(1.0f));
	CHECK(One().y == doctest::Approx(1.0f));

	CHECK(Up().x == doctest::Approx(0.0f));
	CHECK(Up().y == doctest::Approx(1.0f));

	CHECK(Bottom().x == doctest::Approx(0.0f));
	CHECK(Bottom().y == doctest::Approx(-1.0f));

	CHECK(Left().x == doctest::Approx(-1.0f));
	CHECK(Left().y == doctest::Approx(0.0f));

	CHECK(Right().x == doctest::Approx(1.0f));
	CHECK(Right().y == doctest::Approx(0.0f));
}

TEST_CASE("RandomInCircle and RandomOnCircle respect the requested radius")
{
	// The angle is random, but the radius constraint is deterministic.
	const float radius = 3.5f;
	for (int i = 0; i < 64; ++i)
	{
		CHECK(Length(RandomInCircle(radius)) <= doctest::Approx(radius));
		CHECK(Length(RandomOnCircle(radius)) == doctest::Approx(radius));
	}
}
