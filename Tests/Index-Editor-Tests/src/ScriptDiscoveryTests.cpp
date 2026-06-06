// Tests for Index::EditorScriptDiscovery — the editor's header-only script
// scanner that classifies project .cs / native .cpp files into the "Add
// Component" / script pickers. The pure string + token helpers and the
// file-parsing helpers are exercised directly; the cached, ProjectManager-
// driven CollectProjectScriptEntries() is intentionally left out (it depends
// on global project state and a 1s time cache).

#include <doctest/doctest.h>

#include "Scripting/ScriptDiscovery.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Index;
using namespace Index::EditorScriptDiscovery;

namespace {
	// Unique temp directory torn down on scope exit, so file-parsing cases
	// don't collide between runs.
	struct ScopedTempDir {
		std::filesystem::path Root;
		ScopedTempDir() {
			const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
			Root = std::filesystem::temp_directory_path() / ("IndexScriptDiscovery_" + std::to_string(suffix));
			std::filesystem::create_directories(Root);
		}
		~ScopedTempDir() {
			std::error_code ec;
			std::filesystem::remove_all(Root, ec);
		}
		std::filesystem::path Write(const std::string& name, const std::string& contents) const {
			const std::filesystem::path p = Root / name;
			std::ofstream out(p, std::ios::binary);
			out << contents;
			out.close();
			return p;
		}
	};
}

TEST_CASE("EditorScriptDiscovery::ToLowerCopy lowercases without touching non-letters") {
	CHECK(ToLowerCopy("HeLLo_World.CS") == "hello_world.cs");
	CHECK(ToLowerCopy("") == "");
	CHECK(ToLowerCopy("123-ABC") == "123-abc");
}

TEST_CASE("EditorScriptDiscovery::TrimWhitespace strips both ends only") {
	CHECK(TrimWhitespace("  hello  ") == "hello");
	CHECK(TrimWhitespace("\t\n value \r\n") == "value");
	CHECK(TrimWhitespace("a b") == "a b");   // interior preserved
	CHECK(TrimWhitespace("   ").empty());     // all-whitespace -> empty
	CHECK(TrimWhitespace("").empty());
}

TEST_CASE("EditorScriptDiscovery extension predicates are case-insensitive") {
	CHECK(IsCSharpScriptExtension(".cs"));
	CHECK(IsCSharpScriptExtension(".CS"));
	CHECK_FALSE(IsCSharpScriptExtension(".cpp"));
	CHECK_FALSE(IsCSharpScriptExtension("cs"));   // missing dot

	CHECK(IsNativeScriptSourceExtension(".cpp"));
	CHECK(IsNativeScriptSourceExtension(".CC"));
	CHECK(IsNativeScriptSourceExtension(".cxx"));
	CHECK_FALSE(IsNativeScriptSourceExtension(".cs"));
	CHECK_FALSE(IsNativeScriptSourceExtension(".h"));
}

TEST_CASE("EditorScriptDiscovery::IsNativeScriptBootstrapFile matches only the exports TU") {
	CHECK(IsNativeScriptBootstrapFile("NativeScriptExports.cpp"));
	CHECK(IsNativeScriptBootstrapFile(std::filesystem::path("some/dir/NativeScriptExports.cpp")));
	CHECK_FALSE(IsNativeScriptBootstrapFile("PlayerController.cpp"));
}

TEST_CASE("EditorScriptDiscovery::IsValidRegisteredScriptToken accepts identifiers and :: qualifiers") {
	CHECK(IsValidRegisteredScriptToken("PlayerController"));
	CHECK(IsValidRegisteredScriptToken("My::Namespaced::Script"));
	CHECK(IsValidRegisteredScriptToken("_underscore9"));
	CHECK_FALSE(IsValidRegisteredScriptToken(""));
	CHECK_FALSE(IsValidRegisteredScriptToken("has space"));
	CHECK_FALSE(IsValidRegisteredScriptToken("has-dash"));
}

TEST_CASE("EditorScriptDiscovery::SanitizeIdentifier keeps identifier chars, guards leading digit, falls back") {
	CHECK(SanitizeIdentifier("Hello World!", "Fallback") == "HelloWorld");
	CHECK(SanitizeIdentifier("9Lives", "Fallback") == "_9Lives");   // leading digit -> prefix _
	CHECK(SanitizeIdentifier("keep_this_1", "Fallback") == "keep_this_1");
	CHECK(SanitizeIdentifier("@#$%", "Fallback") == "Fallback");    // nothing usable -> fallback
	CHECK(SanitizeIdentifier("", "Default") == "Default");
}

TEST_CASE("EditorScriptDiscovery::SourceHasBaseClass matches short, qualified, and global forms") {
	CHECK(SourceHasBaseClass("classFoo:Component{}", "Component"));
	CHECK(SourceHasBaseClass("classFoo:Index.Component{}", "Component"));
	CHECK(SourceHasBaseClass("classFoo:global::Index.Component{}", "Component"));
	CHECK_FALSE(SourceHasBaseClass("classFoo:Componentish{}", "ComponentX"));
	CHECK_FALSE(SourceHasBaseClass("classFoo{}", "Component"));
}

TEST_CASE("EditorScriptDiscovery::ReadSourceWithoutWhitespace concatenates non-space bytes") {
	ScopedTempDir dir;
	const auto path = dir.Write("Sample.cs", "public class Foo\n{\n\tint x;\n}\n");
	CHECK(ReadSourceWithoutWhitespace(path) == "publicclassFoo{intx;}");

	// Missing file returns empty, never throws.
	CHECK(ReadSourceWithoutWhitespace(dir.Root / "DoesNotExist.cs").empty());
}

TEST_CASE("EditorScriptDiscovery::ContainsScriptEntry / AppendScriptEntry dedupe on (name,type)") {
	std::vector<ScriptEntry> entries;

	AppendScriptEntry(entries, "Player", "Player.cs", ".cs", ScriptType::Managed);
	CHECK(entries.size() == 1);
	CHECK(ContainsScriptEntry(entries, "Player", ScriptType::Managed));
	CHECK_FALSE(ContainsScriptEntry(entries, "Player", ScriptType::Native));

	// Same (name,type) is ignored; an empty name or Unknown type is ignored.
	AppendScriptEntry(entries, "Player", "Player.cs", ".cs", ScriptType::Managed);
	AppendScriptEntry(entries, "", "Empty.cs", ".cs", ScriptType::Managed);
	AppendScriptEntry(entries, "Ignored", "Ignored.cs", ".cs", ScriptType::Unknown);
	CHECK(entries.size() == 1);

	// Same name but a different type IS a distinct entry.
	AppendScriptEntry(entries, "Player", "Player.cpp", ".cpp", ScriptType::Native);
	CHECK(entries.size() == 2);
	CHECK(ContainsScriptEntry(entries, "Player", ScriptType::Native));
}

TEST_CASE("EditorScriptDiscovery::ExtractRegisteredNativeScriptClasses pulls REGISTER_SCRIPT names") {
	ScopedTempDir dir;
	const auto path = dir.Write("Scripts.cpp",
		"REGISTER_SCRIPT(PlayerController)\n"
		"REGISTER_SCRIPT( EnemyAI )\n"        // surrounding whitespace trimmed
		"REGISTER_SCRIPT(PlayerController)\n" // duplicate dropped
		"REGISTER_SCRIPT(Game::Boss)\n");     // qualified name kept

	const std::vector<std::string> classes = ExtractRegisteredNativeScriptClasses(path);
	REQUIRE(classes.size() == 3);
	CHECK(classes[0] == "PlayerController");
	CHECK(classes[1] == "EnemyAI");
	CHECK(classes[2] == "Game::Boss");
}

TEST_CASE("EditorScriptDiscovery::ExtractNativeComponentAttributeName reads the attribute string literal") {
	ScopedTempDir dir;

	const auto withAttr = dir.Write("Enemy.cs",
		"[NativeComponent(\"Cool Enemy\")]\n"
		"public class Enemy : IComponent { }\n");
	CHECK(ExtractNativeComponentAttributeName(withAttr) == "Cool Enemy");

	// The bare ":IComponent" token must NOT be mistaken for the attribute.
	const auto noAttr = dir.Write("Plain.cs", "public class Plain : IComponent { }\n");
	CHECK(ExtractNativeComponentAttributeName(noAttr).empty());
}

TEST_CASE("EditorScriptDiscovery::CollectScriptFile classifies managed components") {
	ScopedTempDir dir;
	std::vector<ScriptEntry> entries;

	const auto managed = dir.Write("Movement.cs", "public class Movement : Component { }\n");
	CollectScriptFile(managed, entries);

	REQUIRE(entries.size() == 1);
	CHECK(entries[0].ClassName == "Movement");
	CHECK(entries[0].Type == ScriptType::Managed);
	CHECK(entries[0].IsManagedComponent);
	CHECK_FALSE(entries[0].IsNativeComponent);
}

TEST_CASE("EditorScriptDiscovery::CollectScriptFile classifies native-backed (IComponent) scripts") {
	ScopedTempDir dir;
	std::vector<ScriptEntry> entries;

	const auto native = dir.Write("Health.cs",
		"[NativeComponent(\"Health Bar\")]\n"
		"public class Health : IComponent { }\n");
	CollectScriptFile(native, entries);

	REQUIRE(entries.size() == 1);
	CHECK(entries[0].ClassName == "Health");
	CHECK(entries[0].Type == ScriptType::Managed);   // still a .cs (managed) source file
	CHECK(entries[0].IsNativeComponent);
	CHECK_FALSE(entries[0].IsManagedComponent);       // IComponent is mutually exclusive with Component
	CHECK(entries[0].NativeName == "Health Bar");
}

TEST_CASE("EditorScriptDiscovery::CollectScriptFile skips the native exports bootstrap TU") {
	ScopedTempDir dir;
	std::vector<ScriptEntry> entries;

	const auto exports = dir.Write("NativeScriptExports.cpp", "REGISTER_SCRIPT(ShouldBeIgnored)\n");
	CollectScriptFile(exports, entries);
	CHECK(entries.empty());
}

TEST_CASE("EditorScriptDiscovery::CollectScriptFiles walks a directory tree and SortScriptEntries orders by name") {
	ScopedTempDir dir;
	dir.Write("Zebra.cs", "public class Zebra : Component { }\n");
	dir.Write("Alpha.cs", "public class Alpha : Component { }\n");

	std::vector<ScriptEntry> entries;
	CollectScriptFiles(dir.Root, entries);
	REQUIRE(entries.size() == 2);

	SortScriptEntries(entries);
	CHECK(entries[0].ClassName == "Alpha");
	CHECK(entries[1].ClassName == "Zebra");

	// A non-existent directory is a clean no-op.
	std::vector<ScriptEntry> none;
	CollectScriptFiles(dir.Root / "missing", none);
	CHECK(none.empty());
}
