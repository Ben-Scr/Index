// Tests for Index::EditorCamera — the editor viewport's orthographic 2D
// camera. Pure glm math (no ImGui / GPU): zoom clamping, mouse-pan world
// mapping, the view-projection matrix, and the visible-region AABB.

#include <doctest/doctest.h>

#include "Editor/EditorCamera.hpp"

#include <glm/glm.hpp>

using namespace Index;

TEST_CASE("EditorCamera defaults: origin position, ortho size 5, zoom 1") {
	EditorCamera camera;
	CHECK(camera.GetPosition().x == doctest::Approx(0.0f));
	CHECK(camera.GetPosition().y == doctest::Approx(0.0f));
	CHECK(camera.GetOrthographicSize() == doctest::Approx(5.0f));
	CHECK(camera.GetZoom() == doctest::Approx(1.0f));
}

TEST_CASE("EditorCamera setters round-trip through getters") {
	EditorCamera camera;
	camera.SetPosition({ 3.0f, -4.0f });
	camera.SetOrthographicSize(12.0f);
	camera.SetZoom(2.0f);

	CHECK(camera.GetPosition().x == doctest::Approx(3.0f));
	CHECK(camera.GetPosition().y == doctest::Approx(-4.0f));
	CHECK(camera.GetOrthographicSize() == doctest::Approx(12.0f));
	CHECK(camera.GetZoom() == doctest::Approx(2.0f));
}

TEST_CASE("EditorCamera::SetViewportSize ignores non-positive dimensions") {
	EditorCamera camera;
	camera.SetViewportSize(800, 600);

	// A zero/negative size must be a no-op (it would divide-by-zero the aspect).
	CHECK_NOTHROW(camera.SetViewportSize(0, 600));
	CHECK_NOTHROW(camera.SetViewportSize(800, -1));

	// 800x600 is still in effect: a square half-extent of ortho*zoom in Y,
	// widened by the 4:3 aspect in X.
	camera.SetOrthographicSize(5.0f);
	const AABB view = camera.GetViewportAABB();
	CHECK(view.Max.y == doctest::Approx(5.0f));
	CHECK(view.Max.x == doctest::Approx(5.0f * (800.0f / 600.0f)));
}

TEST_CASE("EditorCamera::Update does nothing when the viewport is not hovered") {
	EditorCamera camera;
	camera.SetViewportSize(100, 100);

	camera.Update(0.016f, /*isHovered*/ false, Vec2{ 50.0f, 50.0f }, /*scroll*/ 5.0f);

	CHECK(camera.GetPosition().x == doctest::Approx(0.0f));
	CHECK(camera.GetPosition().y == doctest::Approx(0.0f));
	CHECK(camera.GetOrthographicSize() == doctest::Approx(5.0f));
}

TEST_CASE("EditorCamera::Update scroll zooms about the current ortho size") {
	EditorCamera camera;
	camera.SetViewportSize(100, 100);

	// new = ortho * (1 - 0.1*scroll). scroll=+1 from 5 -> 4.5 (zoom in).
	camera.Update(0.0f, true, Vec2{ 0.0f, 0.0f }, 1.0f);
	CHECK(camera.GetOrthographicSize() == doctest::Approx(4.5f));

	// Negative scroll zooms out.
	camera.SetOrthographicSize(5.0f);
	camera.Update(0.0f, true, Vec2{ 0.0f, 0.0f }, -1.0f);
	CHECK(camera.GetOrthographicSize() == doctest::Approx(5.5f));
}

TEST_CASE("EditorCamera::Update clamps zoom to the documented bounds") {
	EditorCamera camera;
	camera.SetViewportSize(100, 100);

	// A huge positive scroll drives ortho size to the lower clamp.
	camera.Update(0.0f, true, Vec2{ 0.0f, 0.0f }, 1000.0f);
	CHECK(camera.GetOrthographicSize() == doctest::Approx(EditorCamera::k_MinOrthographicSize));

	// A huge negative scroll drives it to the upper clamp.
	camera.SetOrthographicSize(5.0f);
	camera.Update(0.0f, true, Vec2{ 0.0f, 0.0f }, -100000.0f);
	CHECK(camera.GetOrthographicSize() == doctest::Approx(EditorCamera::k_MaxOrthographicSize));
}

TEST_CASE("EditorCamera::Update pans in world units derived from the viewport") {
	EditorCamera camera;
	camera.SetViewportSize(100, 100);   // aspect 1
	camera.SetOrthographicSize(5.0f);   // worldHeight = 2*5*zoom = 10
	camera.SetZoom(1.0f);

	// pixelsToWorld = worldSize / viewportSize = 10 / 100 = 0.1 per pixel.
	// Position.x -= dx * 0.1 ; Position.y += dy * 0.1.
	camera.Update(0.0f, true, Vec2{ 10.0f, 20.0f }, 0.0f);
	CHECK(camera.GetPosition().x == doctest::Approx(-1.0f));
	CHECK(camera.GetPosition().y == doctest::Approx(2.0f));
}

TEST_CASE("EditorCamera::GetViewProjectionMatrix maps camera-space corners to NDC") {
	EditorCamera camera;
	camera.SetViewportSize(100, 100);   // aspect 1 -> halfW == halfH
	camera.SetOrthographicSize(5.0f);   // halfExtent = ortho*zoom = 5
	camera.SetZoom(1.0f);
	camera.SetPosition({ 0.0f, 0.0f });

	const glm::mat4 vp = camera.GetViewProjectionMatrix();

	// The camera centre maps to the NDC origin.
	const glm::vec4 centre = vp * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	CHECK(centre.x == doctest::Approx(0.0f));
	CHECK(centre.y == doctest::Approx(0.0f));

	// The top-right of the visible region (+halfExtent, +halfExtent) maps to (1, 1).
	const glm::vec4 corner = vp * glm::vec4(5.0f, 5.0f, 0.0f, 1.0f);
	CHECK(corner.x == doctest::Approx(1.0f));
	CHECK(corner.y == doctest::Approx(1.0f));
}

TEST_CASE("EditorCamera::GetViewportAABB is centred on the camera with ortho half-extents") {
	EditorCamera camera;
	camera.SetViewportSize(200, 100);   // aspect 2
	camera.SetOrthographicSize(4.0f);
	camera.SetZoom(1.0f);
	camera.SetPosition({ 10.0f, -3.0f });

	const AABB view = camera.GetViewportAABB();
	// halfH = ortho*zoom = 4 ; halfW = halfH*aspect = 8.
	CHECK(view.Min.x == doctest::Approx(10.0f - 8.0f));
	CHECK(view.Max.x == doctest::Approx(10.0f + 8.0f));
	CHECK(view.Min.y == doctest::Approx(-3.0f - 4.0f));
	CHECK(view.Max.y == doctest::Approx(-3.0f + 4.0f));
}
