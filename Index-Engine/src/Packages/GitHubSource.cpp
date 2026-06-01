#include "pch.hpp"
#include "Packages/GitHubSource.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/Directory.hpp"
#include "Utils/Process.hpp"

#include <algorithm>
#include <fstream>
#include <filesystem>

namespace Index {

	namespace {
		std::string XmlEscape(std::string_view value) {
			std::string out;
			out.reserve(value.size());
			for (char c : value) {
				switch (c) {
					case '&':  out += "&amp;";  break;
					case '<':  out += "&lt;";   break;
					case '>':  out += "&gt;";   break;
					case '"':  out += "&quot;"; break;
					case '\'': out += "&apos;"; break;
					default:   out.push_back(c); break;
				}
			}
			return out;
		}

		bool IsSafePathComponent(std::string_view value) {
			if (value.empty()) return false;
			if (value == "." || value == "..") return false;
			for (char c : value) {
				const unsigned char uc = static_cast<unsigned char>(c);
				if (uc < 0x20 || uc == 0x7F) return false;
				if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
				    c == '"' || c == '<' || c == '>' || c == '|') return false;
			}
			// Also forbid embedded ".." substrings even if the whole string
			// isn't exactly "..".
			if (value.find("..") != std::string_view::npos) return false;
			return true;
		}

		bool IsSha256Hex(std::string_view value) {
			return value.size() == 64
				&& std::all_of(value.begin(), value.end(), [](unsigned char c) {
					return std::isxdigit(c) != 0;
				});
		}

		bool IsContainedPath(const std::filesystem::path& root,
			const std::filesystem::path& candidate)
		{
			std::error_code ec;
			const std::filesystem::path canonicalRoot =
				std::filesystem::weakly_canonical(root, ec);
			if (ec || canonicalRoot.empty()) return false;

			const std::filesystem::path canonicalCandidate =
				std::filesystem::weakly_canonical(candidate, ec);
			if (ec || canonicalCandidate.empty()) return false;

			const std::filesystem::path relative =
				canonicalCandidate.lexically_relative(canonicalRoot);
			if (relative.empty() || relative.is_absolute()) return false;
			for (const auto& component : relative) {
				if (component == "..") return false;
			}
			return true;
		}

		static bool AddReferenceToProject(const std::string& csprojPath, const std::string& dllPath,
			const std::string& hintPath) {
			std::ifstream in(csprojPath);
			if (!in.is_open()) return false;
			std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			in.close();

			// Check if reference already exists
			if (content.find(hintPath) != std::string::npos)
				return true;

			const std::string stem = std::filesystem::path(dllPath).stem().string();
			std::string refBlock =
				"  <ItemGroup>\n"
				"    <Reference Include=\"" + XmlEscape(stem) + "\">\n"
				"      <HintPath>" + XmlEscape(hintPath) + "</HintPath>\n"
				"    </Reference>\n"
				"  </ItemGroup>\n";

			// Insert before </Project>
			auto pos = content.rfind("</Project>");
			if (pos == std::string::npos) return false;

			content.insert(pos, refBlock);

			if (!File::WriteAllText(csprojPath, content)) {
				IDX_CORE_ERROR_TAG("IndexPackages", "Failed to write csproj after adding reference: {}", csprojPath);
				return false;
			}
			return true;
		}

		// Removes only the matched <Reference> element, not the entire enclosing <ItemGroup> (avoids deleting sibling references).
		static bool RemoveReferenceFromProject(const std::string& csprojPath, const std::string& assemblyName) {
			std::ifstream in(csprojPath);
			if (!in.is_open()) return false;
			std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			in.close();

			const std::string includeFragment = "Include=\"" + XmlEscape(assemblyName) + "\"";
			size_t scanFrom = 0;
			size_t refStart = std::string::npos;
			while (scanFrom < content.size()) {
				const size_t tagStart = content.find("<Reference ", scanFrom);
				if (tagStart == std::string::npos) break;
				const size_t tagClose = content.find('>', tagStart);
				if (tagClose == std::string::npos) break;
				if (content.find(includeFragment, tagStart) < tagClose) {
					refStart = tagStart;
					break;
				}
				scanFrom = tagClose + 1;
			}

			if (refStart == std::string::npos) return true; // already removed

			const size_t openTagClose = content.find('>', refStart);
			if (openTagClose == std::string::npos) return false;
			size_t refEnd = std::string::npos;
			if (openTagClose > 0 && content[openTagClose - 1] == '/') {
				refEnd = openTagClose + 1;
			}
			else {
				constexpr std::string_view kClose = "</Reference>";
				const size_t closePos = content.find(kClose, openTagClose);
				if (closePos == std::string::npos) return false;
				refEnd = closePos + kClose.size();
			}

			if (refEnd < content.size() && content[refEnd] == '\n') refEnd++;

			content.erase(refStart, refEnd - refStart);

			const size_t itemGroupOpen = content.rfind("<ItemGroup>", refStart);
			if (itemGroupOpen != std::string::npos) {
				const size_t itemGroupClose = content.find("</ItemGroup>", itemGroupOpen);
				if (itemGroupClose != std::string::npos) {
					const size_t bodyStart = itemGroupOpen + std::string_view("<ItemGroup>").size();
					const std::string body = content.substr(bodyStart, itemGroupClose - bodyStart);
					if (std::all_of(body.begin(), body.end(), [](unsigned char c) { return std::isspace(c) != 0; })) {
						size_t eraseEnd = itemGroupClose + std::string_view("</ItemGroup>").size();
						if (eraseEnd < content.size() && content[eraseEnd] == '\n') eraseEnd++;
						content.erase(itemGroupOpen, eraseEnd - itemGroupOpen);
					}
				}
			}

			if (!File::WriteAllText(csprojPath, content)) {
				IDX_CORE_ERROR_TAG("IndexPackages", "Failed to write csproj after removing reference: {}", csprojPath);
				return false;
			}
			return true;
		}
	}

	GitHubSource::GitHubSource(const std::string& toolExePath, const std::string& indexUrl,
		const std::string& displayName)
		: m_ToolExePath(toolExePath)
		, m_IndexUrl(indexUrl)
		, m_DisplayName(displayName) {
	}

	std::string GitHubSource::RunTool(const std::vector<std::string>& args) const {
		std::vector<std::string> command;
		command.reserve(args.size() + 2);
		if (std::filesystem::path(m_ToolExePath).extension() == ".dll") {
			command.push_back("dotnet");
		}
		command.push_back(m_ToolExePath);
		command.insert(command.end(), args.begin(), args.end());

		Process::Result result = Process::Run(command);
		if (!result.Succeeded()) {
			IDX_CORE_ERROR_TAG("GitHubSource", "Failed to run tool (exit code {})", result.ExitCode);
		}
		return result.Output;
	}

	void GitHubSource::EnsureIndex() {
		if (m_IndexLoaded) return;

		IDX_CORE_INFO_TAG("GitHubSource", "Fetching package index from {}", m_IndexUrl);
		std::string json = RunTool({ "github-index", m_IndexUrl });

		if (json.empty()) {
			IDX_CORE_WARN_TAG("GitHubSource", "Failed to fetch package index");
			m_IndexLoaded = true;
			return;
		}

		m_CachedIndex.clear();
		Json::Value root;
		std::string parseError;
		if (!Json::TryParse(json, root, &parseError) || !root.IsObject()) {
			IDX_CORE_WARN_TAG("GitHubSource", "Failed to parse index: {}", parseError);
			m_IndexLoaded = true;
			return;
		}

		const Json::Value* packagesValue = root.FindMember("packages");
		if (!packagesValue || !packagesValue->IsArray()) {
			IDX_CORE_WARN_TAG("GitHubSource", "Package index has no 'packages' array");
			m_IndexLoaded = true;
			return;
		}

		m_CachedIndex.reserve(packagesValue->GetArray().size());

		for (const Json::Value& item : packagesValue->GetArray()) {
			if (!item.IsObject()) {
				continue;
			}

			PackageInfo info;
			if (const Json::Value* idValue = item.FindMember("id")) info.Id = idValue->AsStringOr();
			if (const Json::Value* versionValue = item.FindMember("version")) info.Version = versionValue->AsStringOr();
			if (const Json::Value* descriptionValue = item.FindMember("description")) info.Description = descriptionValue->AsStringOr();
			if (const Json::Value* authorsValue = item.FindMember("authors")) info.Authors = authorsValue->AsStringOr();
			info.SourceName = m_DisplayName;
			info.SourceType = PackageSourceType::GitHub;

			if (!info.Id.empty())
				m_CachedIndex.push_back(std::move(info));
		}

		m_IndexLoaded = true;
		IDX_CORE_INFO_TAG("GitHubSource", "Loaded {} engine packages", m_CachedIndex.size());
	}

	std::vector<PackageInfo> GitHubSource::Search(const std::string& query, int take) {
		EnsureIndex();

		if (query.empty())
			return m_CachedIndex;

		std::vector<PackageInfo> results;
		std::string lowerQuery = query;
		std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

		for (const auto& pkg : m_CachedIndex) {
			std::string lowerId = pkg.Id;
			std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(), ::tolower);
			std::string lowerDesc = pkg.Description;
			std::transform(lowerDesc.begin(), lowerDesc.end(), lowerDesc.begin(), ::tolower);

			if (lowerId.find(lowerQuery) != std::string::npos ||
				lowerDesc.find(lowerQuery) != std::string::npos) {
				results.push_back(pkg);
				if (static_cast<int>(results.size()) >= take) break;
			}
		}

		return results;
	}

	PackageOperationResult GitHubSource::Install(const std::string& packageId,
		const std::string& version, const std::string& csprojPath) {

		EnsureIndex();

		// Find the package in the index
		const PackageInfo* found = nullptr;
		for (const auto& pkg : m_CachedIndex) {
			if (pkg.Id == packageId) {
				found = &pkg;
				break;
			}
		}

		if (!found) {
			return { false, "Package '" + packageId + "' not found in index" };
		}

		// Re-fetch the full index entry to get distributionType and dllUrl
		std::string json = RunTool({ "github-index", m_IndexUrl });
		Json::Value root;
		std::string parseError;
		if (!Json::TryParse(json, root, &parseError) || !root.IsObject()) {
			return { false, "Failed to parse package index" };
		}

		std::string distType, dllUrl, dllSha256, nugetId;
		if (const Json::Value* packagesValue = root.FindMember("packages")) {
			for (const Json::Value& item : packagesValue->GetArray()) {
				if (!item.IsObject()) {
					continue;
				}
				const Json::Value* idValue = item.FindMember("id");
				if (!idValue || idValue->AsStringOr() != packageId) {
					continue;
				}
				if (const Json::Value* distTypeValue = item.FindMember("distributionType")) distType = distTypeValue->AsStringOr();
				if (const Json::Value* dllUrlValue = item.FindMember("dllUrl")) dllUrl = dllUrlValue->AsStringOr();
				if (const Json::Value* sha256Value = item.FindMember("sha256")) dllSha256 = sha256Value->AsStringOr();
				if (const Json::Value* dllSha256Value = item.FindMember("dllSha256")) dllSha256 = dllSha256Value->AsStringOr();
				if (const Json::Value* nugetIdValue = item.FindMember("nugetPackageId")) nugetId = nugetIdValue->AsStringOr();
				break;
			}
		}

		if (distType == "nuget") {
			std::string id = nugetId.empty() ? packageId : nugetId;
			std::vector<std::string> command = {
				"dotnet",
				"add",
				csprojPath,
				"package",
				id
			};
			if (!version.empty()) {
				command.push_back("--version");
				command.push_back(version);
			}

			// MUST run from .csproj's dir: dotnet resolves the project relative to CWD, not the path arg, so running from bin/ produces "no project found".
			std::filesystem::path projectDir = std::filesystem::path(csprojPath).parent_path();
			Process::Result addResult = Process::Run(command, projectDir);
			if (!addResult.Succeeded())
				return { false, "dotnet add package failed (exit code " + std::to_string(addResult.ExitCode) + ")" };

			return { true, packageId + " installed via NuGet" };
		}
		else if (distType == "dll") {
			if (dllUrl.empty())
				return { false, "Package '" + packageId + "' has no download URL" };
			if (!IsSha256Hex(dllSha256))
				return { false, "Package '" + packageId + "' has no valid SHA-256 digest" };

			IndexProject* project = ProjectManager::GetCurrentProject();
			if (!project)
				return { false, "No project loaded" };

			if (!IsSafePathComponent(packageId)) {
				return { false, "Refusing to install package with unsafe id: '" + packageId + "'" };
			}
			if (!version.empty() && !IsSafePathComponent(version)) {
				return { false, "Refusing to install package with unsafe version: '" + version + "'" };
			}

			std::string packagesDir = Path::Combine(project->RootDirectory, "Packages", packageId, version);
			std::string dllName = packageId + ".dll";
			std::string localDll = Path::Combine(packagesDir, dllName);

			const std::filesystem::path packagesRoot =
				std::filesystem::path(project->RootDirectory) / "Packages";
			if (!IsContainedPath(packagesRoot, localDll)) {
				return { false, "Refusing to install package: path escapes Packages root" };
			}

			// Download
			IDX_CORE_INFO_TAG("GitHubSource", "Downloading {} to {}", packageId, localDll);
			std::string dlOutput = RunTool({ "github-download", dllUrl, localDll, dllSha256 });

			Json::Value downloadResult;
			std::string downloadParseError;
			if (!Json::TryParse(dlOutput, downloadResult, &downloadParseError) || !downloadResult.IsObject()) {
				return { false, "Download failed for " + packageId };
			}

			const Json::Value* successValue = downloadResult.FindMember("success");
			if (!successValue || !successValue->AsBoolOr(false))
				return { false, "Download failed for " + packageId };

			std::string hintPath = "Packages/" + packageId + "/" + version + "/" + dllName;
			if (!AddReferenceToProject(csprojPath, localDll, hintPath))
				return { false, "Failed to add reference to .csproj" };

			return { true, packageId + " installed (DLL)" };
		}

		return { false, "Unknown distribution type: " + distType };
	}

	PackageOperationResult GitHubSource::Remove(const std::string& packageId,
		const std::string& csprojPath) {

		if (!IsSafePathComponent(packageId)) {
			return { false, "Refusing to remove package with unsafe id: '" + packageId + "'" };
		}

		// Try removing as NuGet package first
		Process::Result removeResult = Process::Run({
			"dotnet",
			"remove",
			csprojPath,
			"package",
			packageId
		});

		if (removeResult.Succeeded())
			return { true, packageId + " removed" };

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project)
			return { false, "No project loaded" };

		const std::filesystem::path packagesRoot =
			std::filesystem::path(project->RootDirectory) / "Packages";
		const std::filesystem::path packagesDir = packagesRoot / packageId;
		if (!IsContainedPath(packagesRoot, packagesDir)) {
			return { false, "Refusing to remove package: path escapes Packages root" };
		}

		// If that fails, try removing as a DLL reference
		if (!RemoveReferenceFromProject(csprojPath, packageId))
			return { false, "Failed to remove " + packageId };

		// Delete local DLL
		if (std::filesystem::exists(packagesDir)) {
			std::error_code ec;
			std::filesystem::remove_all(packagesDir, ec);
			if (ec) {
				return { false, "Failed to remove package files: " + ec.message() };
			}
		}

		return { true, packageId + " removed" };
	}

}
