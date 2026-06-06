#include <doctest/doctest.h>

#include "Collections/AABB.hpp"
#include "Collections/Vec2.hpp"
#include "Math/Trigonometry.hpp"

using namespace Index;

TEST_CASE("Create from center and half-extents yields min = center - half, max = center + half")
{
	const Vec2 center{ 4.0f, -2.0f };
	const Vec2 half{ 3.0f, 5.0f };

	const AABB box = AABB::Create(center, half);

	CHECK(box.Min.x == doctest::Approx(center.x - half.x));
	CHECK(box.Min.y == doctest::Approx(center.y - half.y));
	CHECK(box.Max.x == doctest::Approx(center.x + half.x));
	CHECK(box.Max.y == doctest::Approx(center.y + half.y));
}

TEST_CASE("Create with 0 degrees equals the unrotated Create")
{
	const Vec2 center{ -1.0f, 7.0f };
	const Vec2 half{ 2.0f, 6.0f };

	const AABB unrotated = AABB::Create(center, half);
	const AABB rotated = AABB::Create(center, half, 0.0f);

	CHECK(rotated.Min.x == doctest::Approx(unrotated.Min.x));
	CHECK(rotated.Min.y == doctest::Approx(unrotated.Min.y));
	CHECK(rotated.Max.x == doctest::Approx(unrotated.Max.x));
	CHECK(rotated.Max.y == doctest::Approx(unrotated.Max.y));
}

TEST_CASE("Create with 90 degrees swaps the half-extents")
{
	const Vec2 center{ 10.0f, 20.0f };
	const Vec2 half{ 3.0f, 8.0f };

	// Rotating the box by 90 degrees swaps the axis-aligned bounds' extents.
	const AABB rotated = AABB::Create(center, half, 90.0f);

	CHECK(rotated.Min.x == doctest::Approx(center.x - half.y));
	CHECK(rotated.Min.y == doctest::Approx(center.y - half.x));
	CHECK(rotated.Max.x == doctest::Approx(center.x + half.y));
	CHECK(rotated.Max.y == doctest::Approx(center.y + half.x));
}

TEST_CASE("Intersects is true for overlapping and edge-touching boxes, false for separated")
{
	const AABB a{ Vec2{ 0.0f, 0.0f }, Vec2{ 4.0f, 4.0f } };

	// Overlapping: b's lower-left corner sits inside a.
	const AABB overlapping{ Vec2{ 2.0f, 2.0f }, Vec2{ 6.0f, 6.0f } };
	CHECK(AABB::Intersects(a, overlapping));
	CHECK(AABB::Intersects(overlapping, a)); // symmetric

	// Edge-touching: b's left edge coincides with a's right edge (x == 4).
	const AABB touching{ Vec2{ 4.0f, 0.0f }, Vec2{ 8.0f, 4.0f } };
	CHECK(AABB::Intersects(a, touching));

	// Separated along x with a clear gap.
	const AABB separated{ Vec2{ 5.0f, 0.0f }, Vec2{ 9.0f, 4.0f } };
	CHECK_FALSE(AABB::Intersects(a, separated));

	// Separated along y with a clear gap.
	const AABB separatedY{ Vec2{ 0.0f, 5.0f }, Vec2{ 4.0f, 9.0f } };
	CHECK_FALSE(AABB::Intersects(a, separatedY));
}

TEST_CASE("Contains is true for inside and on-edge points, false for outside points")
{
	const AABB box{ Vec2{ -2.0f, -2.0f }, Vec2{ 2.0f, 2.0f } };

	// Strictly inside.
	CHECK(AABB::Contains(box, Vec2{ 0.0f, 0.0f }));
	CHECK(AABB::Contains(box, Vec2{ 1.5f, -1.0f }));

	// On the boundary (inclusive).
	CHECK(AABB::Contains(box, Vec2{ 2.0f, 0.0f }));   // right edge
	CHECK(AABB::Contains(box, Vec2{ -2.0f, -2.0f }));  // min corner
	CHECK(AABB::Contains(box, Vec2{ 2.0f, 2.0f }));    // max corner

	// Outside on each axis.
	CHECK_FALSE(AABB::Contains(box, Vec2{ 2.5f, 0.0f }));
	CHECK_FALSE(AABB::Contains(box, Vec2{ 0.0f, -3.0f }));
	CHECK_FALSE(AABB::Contains(box, Vec2{ -10.0f, -10.0f }));
}

TEST_CASE("Scale returns Max - Min")
{
	const AABB box{ Vec2{ -1.0f, 3.0f }, Vec2{ 5.0f, 9.0f } };

	const Vec2 scale = box.Scale();
	CHECK(scale.x == doctest::Approx(6.0f));   // 5 - (-1)
	CHECK(scale.y == doctest::Approx(6.0f));   // 9 - 3

	// A box created from half-extents has Scale == 2 * halfExtents.
	const Vec2 half{ 2.0f, 7.0f };
	const AABB fromHalf = AABB::Create(Vec2{ 100.0f, -50.0f }, half);
	const Vec2 fromHalfScale = fromHalf.Scale();
	CHECK(fromHalfScale.x == doctest::Approx(2.0f * half.x));
	CHECK(fromHalfScale.y == doctest::Approx(2.0f * half.y));
}

TEST_CASE("IsAxisAligned is true at the four cardinal angles")
{
	CHECK(AABB::IsAxisAligned(0.0f));
	CHECK(AABB::IsAxisAligned(HalfPi<float>()));
	CHECK(AABB::IsAxisAligned(Pi<float>()));
	CHECK(AABB::IsAxisAligned(3.0f * HalfPi<float>()));
}

TEST_CASE("IsAxisAligned normalizes negative and large angles onto the cardinal set")
{
	// TwoPi normalizes to 0.
	CHECK(AABB::IsAxisAligned(TwoPi<float>()));

	// Negative cardinal angles wrap into [0, TwoPi).
	CHECK(AABB::IsAxisAligned(-HalfPi<float>()));   // -> 3*HalfPi
	CHECK(AABB::IsAxisAligned(-Pi<float>()));       // -> Pi

	// Adding full turns must not change the result.
	CHECK(AABB::IsAxisAligned(HalfPi<float>() + TwoPi<float>()));
	CHECK(AABB::IsAxisAligned(Pi<float>() - TwoPi<float>()));
}

TEST_CASE("IsAxisAligned is false for a 45-degree (off-axis) angle")
{
	CHECK_FALSE(AABB::IsAxisAligned(Radians(45.0f)));
	CHECK_FALSE(AABB::IsAxisAligned(Radians(30.0f)));
	CHECK_FALSE(AABB::IsAxisAligned(HalfPi<float>() * 0.5f)); // 45 degrees
}
