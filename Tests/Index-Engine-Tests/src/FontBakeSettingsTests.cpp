#include <doctest/doctest.h>

#include "Graphics/Text/FontManager.hpp"
#include "Project/IndexProject.hpp"
#include "Serialization/Path.hpp"

#include <chrono>
#include <filesystem>

using namespace Index;

// FontManager::QuantizeBakeSize is the single source of truth shared by the
// slot cache and TextRenderer. These cases lock both the adaptive ladder
// (step == 0) and the fixed-step override the project setting drives. No GPU
// or FontManager::Initialize() needed — it's a pure function over s_BakeStep.
TEST_CASE("FontManager::QuantizeBakeSize adaptive ladder (step 0)") {
	const int saved = FontManager::GetBakeStep();
	FontManager::SetBakeStep(0);

	// <= 16 px: exact integers.
	CHECK(FontManager::QuantizeBakeSize(8.0f) == 8);
	CHECK(FontManager::QuantizeBakeSize(16.0f) == 16);
	// 17..32: snap to 2.
	CHECK(FontManager::QuantizeBakeSize(24.0f) == 24);
	CHECK(FontManager::QuantizeBakeSize(25.0f) == 26);
	// 33..64: snap to 4.
	CHECK(FontManager::QuantizeBakeSize(40.0f) == 40);
	// 65..128: snap to 8.
	CHECK(FontManager::QuantizeBakeSize(100.0f) == 104);
	// > 128: snap to 16.
	CHECK(FontManager::QuantizeBakeSize(200.0f) == 208);

	FontManager::SetBakeStep(saved);
}

TEST_CASE("FontManager::QuantizeBakeSize fixed step override") {
	const int saved = FontManager::GetBakeStep();

	SUBCASE("step 4 snaps to multiples and never below one step") {
		FontManager::SetBakeStep(4);
		CHECK(FontManager::QuantizeBakeSize(1.0f) == 4);    // rounds down to 0 -> clamped to step
		CHECK(FontManager::QuantizeBakeSize(5.0f) == 4);
		CHECK(FontManager::QuantizeBakeSize(6.0f) == 8);
		CHECK(FontManager::QuantizeBakeSize(200.0f) == 200);
	}
	SUBCASE("step 16 collapses small sizes to one bucket") {
		FontManager::SetBakeStep(16);
		CHECK(FontManager::QuantizeBakeSize(8.0f) == 16);
		CHECK(FontManager::QuantizeBakeSize(16.0f) == 16);
		CHECK(FontManager::QuantizeBakeSize(40.0f) == 48);  // snap(40,16) = 48
	}
	SUBCASE("negative step is treated as adaptive") {
		FontManager::SetBakeStep(-5);
		CHECK(FontManager::GetBakeStep() == 0);
		CHECK(FontManager::QuantizeBakeSize(8.0f) == 8);
	}

	FontManager::SetBakeStep(saved);
}

// project.json round-trip for the two new font baking knobs. Mirrors the
// write-if-non-default serialization contract: non-default values survive
// Save() -> Load(), and the clamp on load rejects out-of-range input.
TEST_CASE("IndexProject round-trips font baking settings through project.json") {
	namespace fs = std::filesystem;
	const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path root = fs::temp_directory_path() / ("IndexFontBakeRT_" + std::to_string(suffix));
	fs::create_directories(root);
	struct Cleanup {
		fs::path Root;
		~Cleanup() { std::error_code ec; fs::remove_all(Root, ec); }
	} cleanup{ root };

	SUBCASE("non-default values survive a round-trip") {
		IndexProject p;
		p.Name = "Font Bake RT";
		p.RootDirectory = root.string();
		p.ProjectFilePath = Path::Combine(root.string(), "project.json");
		p.FontBakeStep = 8;
		p.FontAtlasBudgetMB = 64;
		REQUIRE(p.Save());

		IndexProject loaded = IndexProject::Load(root.string());
		CHECK(loaded.FontBakeStep == 8);
		CHECK(loaded.FontAtlasBudgetMB == 64);
	}

	SUBCASE("defaults are omitted and reconstructed") {
		IndexProject p;
		p.Name = "Font Bake Defaults";
		p.RootDirectory = root.string();
		p.ProjectFilePath = Path::Combine(root.string(), "project.json");
		// Leave FontBakeStep = 0 and FontAtlasBudgetMB = 128 (defaults).
		REQUIRE(p.Save());

		IndexProject loaded = IndexProject::Load(root.string());
		CHECK(loaded.FontBakeStep == 0);
		CHECK(loaded.FontAtlasBudgetMB == 128);
	}
}
