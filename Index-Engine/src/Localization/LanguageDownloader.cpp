#include <pch.hpp>
#include "Localization/LanguageDownloader.hpp"

#include "Core/Log.hpp"
#include "Profiling/Profiler.hpp"
#include "Utils/Hash.hpp"
#include "Utils/PackageToolPath.hpp"
#include "Utils/Process.hpp"

#include <fstream>
#include <system_error>
#include <vector>

namespace Index::Localization {

	LanguageDownloader::~LanguageDownloader() {
		if (m_Worker.joinable()) m_Worker.join();
	}

	bool LanguageDownloader::IsRunning() const {
		std::scoped_lock lock(m_Mutex);
		return m_Running;
	}

	DownloadStatusSnapshot LanguageDownloader::Status() const {
		std::scoped_lock lock(m_Mutex);
		DownloadStatusSnapshot s;
		s.Code = m_Code;
		s.Stage = m_Stage;
		s.Progress = m_Progress;
		s.Running = m_Running;
		s.Finished = m_Finished;
		s.Success = m_Success;
		s.RestartRequired = m_RestartRequired;
		s.Error = m_Error;
		return s;
	}

	void LanguageDownloader::Start(std::string code, std::vector<DownloadItem> items, bool restartRequiredOnSuccess) {
		{
			std::scoped_lock lock(m_Mutex);
			if (m_Running) return;
		}

		if (m_Worker.joinable()) m_Worker.join();

		{
			std::scoped_lock lock(m_Mutex);
			m_Code = code;
			m_Stage.clear();
			m_Progress = 0.0f;
			m_Running = true;
			m_Finished = false;
			m_Success = false;
			m_RestartRequired = false;
			m_Error.clear();
		}

		m_Worker = std::thread(
			[this, code = std::move(code), items = std::move(items), restartRequiredOnSuccess]() mutable {
				RunWorker(std::move(code), std::move(items), restartRequiredOnSuccess);
			});
	}

	void LanguageDownloader::RunWorker(std::string code, std::vector<DownloadItem> items, bool restartRequiredOnSuccess) {
		INDEX_PROFILE_THREAD_NAME("LanguageDownloader");
		bool success = true;
		std::string finalError;

		const std::size_t total = items.empty() ? 1 : items.size();
		for (std::size_t i = 0; i < items.size(); ++i) {
			const DownloadItem& item = items[i];
			{
				std::scoped_lock lock(m_Mutex);
				m_Stage = "Downloading " + item.TargetPath.filename().string();
				m_Progress = static_cast<float>(i) / static_cast<float>(total);
			}

			std::string err;
			if (!DownloadOne(item, err)) {
				success = false;
				finalError = err;
				break;
			}
		}

		{
			std::scoped_lock lock(m_Mutex);
			m_Stage = success ? "Done" : "Failed";
			m_Progress = success ? 1.0f : m_Progress;
			m_Running = false;
			m_Finished = true;
			m_Success = success;
			m_RestartRequired = success && restartRequiredOnSuccess;
			m_Error = finalError;
		}
	}

	bool LanguageDownloader::DownloadOne(const DownloadItem& item, std::string& outError) {
		const std::filesystem::path toolPath = Index::PackageTool::ResolveExecutable();
		if (toolPath.empty()) {
			outError = "Index-PackageTool not found";
			return false;
		}

		std::error_code ec;
		std::filesystem::create_directories(item.TargetPath.parent_path(), ec);

		const std::filesystem::path tempPath = item.TargetPath.string() + ".dl.tmp";
		std::filesystem::remove(tempPath, ec);

		std::vector<std::string> command;
		if (toolPath.extension() == ".dll") {
			command.push_back("dotnet");
			command.push_back(toolPath.string());
		}
		else {
			command.push_back(toolPath.string());
		}
		command.push_back("github-download");
		command.push_back(item.Url);
		command.push_back(tempPath.string());

		const Process::Result result = Process::Run(command, {}, std::chrono::milliseconds(0));
		if (!result.Succeeded()) {
			std::filesystem::remove(tempPath, ec);
			outError = "PackageTool exited with code " + std::to_string(result.ExitCode);
			IDX_CORE_WARN_TAG("Localization", "Download failed for {}: {}", item.Url, result.Output);
			return false;
		}

		if (item.ExpectedSizeBytes > 0) {
			const std::uintmax_t actual = std::filesystem::file_size(tempPath, ec);
			if (ec || actual != item.ExpectedSizeBytes) {
				std::filesystem::remove(tempPath, ec);
				outError = "Size mismatch (expected " + std::to_string(item.ExpectedSizeBytes)
					+ ", got " + (ec ? std::string("unreadable") : std::to_string(actual)) + ")";
				return false;
			}
		}

		if (!item.ExpectedSha256.empty()) {
			std::string verifyError;
			if (!VerifySha256(tempPath, item.ExpectedSha256, verifyError)) {
				std::filesystem::remove(tempPath, ec);
				outError = verifyError;
				return false;
			}
		}

		std::filesystem::remove(item.TargetPath, ec);
		std::filesystem::rename(tempPath, item.TargetPath, ec);
		if (ec) {
			outError = "Rename to target failed: " + ec.message();
			return false;
		}

		IDX_CORE_INFO_TAG("Localization", "Downloaded '{}' ({})",
			item.TargetPath.string(), item.Url);
		return true;
	}

	bool LanguageDownloader::VerifySha256(const std::filesystem::path& filePath,
		std::string_view expectedHex, std::string& outError) const {
		std::string actual;
		if (!Hash::Sha256OfFile(filePath, actual, outError)) return false;

		const std::string expected = Hash::ToLowerHex(expectedHex);
		if (actual != expected) {
			outError = "SHA-256 mismatch (expected " + expected + ", got " + actual + ")";
			return false;
		}
		return true;
	}

}
