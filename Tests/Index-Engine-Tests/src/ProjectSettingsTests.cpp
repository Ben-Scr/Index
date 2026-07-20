#include <doctest/doctest.h>

#include "Project/IndexProject.hpp"
#include "Core/Time.hpp"
#include "Serialization/Path.hpp"

#include <chrono>
#include <filesystem>

using namespace Index;

// project.json round-trip for the default physics-world Hz setting. Mirrors the
// write-if-non-default serialization contract: a non-default value survives
// Save() -> Load(), the default is omitted and reconstructed, and the load-time
// clamp keeps out-of-range input inside Time::SetFixedDeltaTime's [1, 240] Hz range.
TEST_CASE("IndexProject round-trips physics tick rate through project.json") {
	namespace fs = std::filesystem;
	const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("IndexPhysicsRT_" + std::to_string(suffix));
	fs::create_directories(root);
	struct Cleanup {
		fs::path Root;
		~Cleanup() { std::error_code ec; fs::remove_all(Root, ec); }
	} cleanup{ root };

	auto makeProject = [&](const char* name) {
		IndexProject p;
		p.Name = name;
		p.RootDirectory = root.string();
		p.ProjectFilePath = Path::Combine(root.string(), "project.json");
		return p;
	};

	SUBCASE("non-default value survives a round-trip") {
		IndexProject p = makeProject("Physics RT");
		p.PhysicsTickRate = 120;
		REQUIRE(p.Save());

		IndexProject loaded = IndexProject::Load(root.string());
		CHECK(loaded.PhysicsTickRate == 120);
	}

	SUBCASE("default is omitted and reconstructed") {
		IndexProject p = makeProject("Physics Defaults");
		// Leave PhysicsTickRate = 50 (default).
		REQUIRE(p.Save());

		IndexProject loaded = IndexProject::Load(root.string());
		CHECK(loaded.PhysicsTickRate == 50);
	}

	SUBCASE("out-of-range value is clamped on load") {
		IndexProject p = makeProject("Physics Clamp");
		p.PhysicsTickRate = 100000; // absurd; loader clamps to the [1, 240] Hz range
		REQUIRE(p.Save());

		IndexProject loaded = IndexProject::Load(root.string());
		CHECK(loaded.PhysicsTickRate == IndexProject::k_MaxPhysicsHz);
	}
}

// Guards the apply path itself, not just serialization: the Hz the panel/startup
// push into Time must round-trip to the expected fixed timestep. The Hz bounds are
// chosen to land exactly on Time::SetFixedDeltaTime's [1/240, 1]s clamp endpoints, so
// this also locks the IndexProject bounds and the Time clamp together.
TEST_CASE("PhysicsTickRate maps to the fixed timestep Time actually uses") {
	auto fixedStepFor = [](int hz) {
		IndexProject p;
		p.PhysicsTickRate = hz;
		Time time;
		time.SetFixedDeltaTime(p.GetFixedDeltaTimeSeconds());
		return time.GetFixedDeltaTime();
	};

	CHECK(fixedStepFor(50) == doctest::Approx(1.0f / 50.0f));
	CHECK(fixedStepFor(60) == doctest::Approx(1.0f / 60.0f));
	// Endpoints sit on Time's clamp bounds, so they pass through unchanged.
	CHECK(fixedStepFor(IndexProject::k_MaxPhysicsHz) == doctest::Approx(1.0f / 240.0f));
	CHECK(fixedStepFor(IndexProject::k_MinPhysicsHz) == doctest::Approx(1.0f));
}
