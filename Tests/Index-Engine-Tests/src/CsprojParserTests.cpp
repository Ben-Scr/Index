// Regression tests for Index::ParseInstalledPackagesFromXml. Add SUBCASEs to an existing TEST_CASE when the fixture is the same; new TEST_CASE when the fixture changes.

#include <doctest/doctest.h>

#include "Packages/CsprojParser.hpp"
#include "Packages/PackageInfo.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

using Index::ParseInstalledPackagesFromXml;
using Index::PackageInfo;
using Index::PackageSourceType;

namespace {
	// Find a package by id (case-sensitive — NuGet IDs as written into the .csproj).
	const PackageInfo* Find(const std::vector<PackageInfo>& v, const std::string& id) {
		for (const auto& p : v) {
			if (p.Id == id) return &p;
		}
		return nullptr;
	}
} // namespace

// REGRESSION: parse_non_destructive left node names un-null-terminated, so comparisons like `s == "PackageReference"` always failed — packages never appeared in the In Project tab.
TEST_CASE("ParseInstalledPackagesFromXml — well-formed PackageReference is detected") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net9.0</TargetFramework>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="RandomCS" Version="1.0.1" />
  </ItemGroup>
</Project>)";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 1);
	CHECK(installed[0].Id == "RandomCS");
	CHECK(installed[0].Version == "1.0.1");
	CHECK(installed[0].InstalledVersion == "1.0.1");
	CHECK(installed[0].IsInstalled);
	CHECK(installed[0].SourceType == PackageSourceType::NuGet);
}

// REGRESSION: after the parse fix, Index-ScriptCore (emitted by the project generator) leaked into the User Packages panel.
TEST_CASE("ParseInstalledPackagesFromXml — engine ScriptCore reference is filtered") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <Reference Include="Index-ScriptCore">
      <HintPath>Packages\Index-ScriptCore\Index-ScriptCore.dll</HintPath>
      <Private>true</Private>
    </Reference>
  </ItemGroup>
  <ItemGroup>
    <PackageReference Include="RandomCS" Version="1.0.1" />
    <PackageReference Include="RandomNameGenerator" Version="1.0.4" />
  </ItemGroup>
</Project>)";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 2);
	CHECK(Find(installed, "Index-ScriptCore") == nullptr);
	CHECK(Find(installed, "RandomCS") != nullptr);
	CHECK(Find(installed, "RandomNameGenerator") != nullptr);
}

TEST_CASE("ParseInstalledPackagesFromXml — system references are filtered") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <Reference Include="System.Text.Json" />
    <Reference Include="Microsoft.Extensions.Logging" />
    <Reference Include="MyUserDll">
      <HintPath>libs/MyUserDll.dll</HintPath>
    </Reference>
  </ItemGroup>
</Project>)";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 1);
	CHECK(installed[0].Id == "MyUserDll");
	CHECK(installed[0].SourceType == PackageSourceType::GitHub); // <Reference> source bucket
}

// Central package management omits the Version attribute; parser must fall back to centralVersions map.
TEST_CASE("ParseInstalledPackagesFromXml — central package version is resolved") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <PackageReference Include="Newtonsoft.Json" />
  </ItemGroup>
</Project>)";

	const std::unordered_map<std::string, std::string> central = {
		{ "Newtonsoft.Json", "13.0.3" }
	};

	auto installed = ParseInstalledPackagesFromXml(xml, central);

	REQUIRE(installed.size() == 1);
	CHECK(installed[0].Id == "Newtonsoft.Json");
	CHECK(installed[0].Version == "13.0.3");
	CHECK(installed[0].InstalledVersion == "13.0.3");
}

// Called on every poll; must not throw on a half-written .csproj — empty result is the contract.
TEST_CASE("ParseInstalledPackagesFromXml — malformed input returns empty without throwing") {
	SUBCASE("not XML at all") {
		auto installed = ParseInstalledPackagesFromXml("this is not xml");
		CHECK(installed.empty());
	}
	SUBCASE("truncated mid-element") {
		auto installed = ParseInstalledPackagesFromXml("<Project><ItemGroup><PackageReference Include=\"Foo");
		CHECK(installed.empty());
	}
	SUBCASE("empty string") {
		auto installed = ParseInstalledPackagesFromXml("");
		CHECK(installed.empty());
	}
	SUBCASE("xml with no Project root") {
		auto installed = ParseInstalledPackagesFromXml("<NotAProject><ItemGroup/></NotAProject>");
		CHECK(installed.empty());
	}
}

// dotnet add appends a new <ItemGroup> per install; all PackageReferences across all ItemGroups must be aggregated.
TEST_CASE("ParseInstalledPackagesFromXml — packages across multiple ItemGroups are collected") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <Compile Include="Assets\**\*.cs" />
  </ItemGroup>
  <ItemGroup>
    <PackageReference Include="A" Version="1.0.0" />
  </ItemGroup>
  <ItemGroup>
    <PackageReference Include="B" Version="2.0.0" />
  </ItemGroup>
  <ItemGroup>
    <PackageReference Include="C" Version="3.0.0" />
  </ItemGroup>
</Project>)";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 3);
	CHECK(Find(installed, "A")->Version == "1.0.0");
	CHECK(Find(installed, "B")->Version == "2.0.0");
	CHECK(Find(installed, "C")->Version == "3.0.0");
}

// Hand-edited .csproj may use child <Version> element instead of the Version attribute; documented as supported.
TEST_CASE("ParseInstalledPackagesFromXml — child <Version> element is honored") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <PackageReference Include="HandWritten">
      <Version>9.9.9</Version>
    </PackageReference>
  </ItemGroup>
</Project>)";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 1);
	CHECK(installed[0].Id == "HandWritten");
	CHECK(installed[0].Version == "9.9.9");
}

// End-to-end: exact shape IndexProject.cpp emits. R"xml(...)xml" delimiter because the fixture contains `)"` which would end a default R"(...)" string.
TEST_CASE("ParseInstalledPackagesFromXml — real Index-generated .csproj layout") {
	const std::string xml = R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Library</OutputType>
    <TargetFramework>net9.0</TargetFramework>
    <Configurations>Debug;Release;Dist</Configurations>
  </PropertyGroup>

  <ItemGroup>
    <Compile Include="Assets\**\*.cs" />
  </ItemGroup>

  <ItemGroup>
    <Reference Include="Index-ScriptCore">
      <HintPath>Packages\Index-ScriptCore\Index-ScriptCore.dll</HintPath>
      <Private>true</Private>
    </Reference>
  </ItemGroup>

  <ItemGroup>
    <PackageReference Include="RandomCS" Version="1.0.1" />
    <PackageReference Include="RandomNameGenerator" Version="1.0.4" />
  </ItemGroup>

  <!-- Auto-generated by Index Package Manager every time a package is installed/removed. -->
  <Import Project="Packages/IndexPackages.props" Condition="Exists('Packages/IndexPackages.props')" />

</Project>)xml";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 2);
	CHECK(Find(installed, "Index-ScriptCore") == nullptr);

	const PackageInfo* randomCs = Find(installed, "RandomCS");
	REQUIRE(randomCs != nullptr);
	CHECK(randomCs->Version == "1.0.1");
	CHECK(randomCs->IsInstalled);
	CHECK(randomCs->SourceType == PackageSourceType::NuGet);

	const PackageInfo* rng = Find(installed, "RandomNameGenerator");
	REQUIRE(rng != nullptr);
	CHECK(rng->Version == "1.0.4");
	CHECK(rng->IsInstalled);
}
