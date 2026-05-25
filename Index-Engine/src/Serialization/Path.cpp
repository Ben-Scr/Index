#include <pch.hpp>
#include "Path.hpp"

#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"

#include <optional>

#ifdef IDX_PLATFORM_WINDOWS
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <unistd.h>
#include <limits.h>
#endif

namespace Index {
    std::string Path::GetSpecialFolderPath(SpecialFolder folder)
    {
#ifdef IDX_PLATFORM_WINDOWS
        PWSTR path = nullptr;
        HRESULT hr = E_FAIL;

        switch (folder) {
        case SpecialFolder::User: hr = SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &path); break;
        case SpecialFolder::Desktop: hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path); break;
        case SpecialFolder::Documents: hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path); break;
        case SpecialFolder::Downloads: hr = SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path); break;
        case SpecialFolder::Music: hr = SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &path); break;
        case SpecialFolder::Pictures: hr = SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &path); break;
        case SpecialFolder::Videos: hr = SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &path); break;
        case SpecialFolder::AppDataRoaming: hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path); break;
        case SpecialFolder::LocalAppData: hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path); break;
        case SpecialFolder::ProgramData: hr = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path); break;
        default: IDX_THROW(IndexErrorCode::InvalidArgument, "Unknown SpecialFolder");
        }

        IDX_ASSERT(!FAILED(hr), IndexErrorCode::Undefined, "Failed to get folder path");

        const int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
        // size==0 means WideCharToMultiByte failed; without a guard, std::string(size-1,...)
        // becomes std::string(SIZE_MAX, 0) and crashes the process.
        if (size <= 0) {
            CoTaskMemFree(path);
            return {};
        }
        std::string result(size - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, path, -1, &result[0], size, nullptr, nullptr);

        CoTaskMemFree(path);
        return result;
#else
        const char* home = std::getenv("HOME");
        if (!home)
            IDX_THROW(IndexErrorCode::Undefined, "HOME environment variable is not set");

        switch (folder) {
        case SpecialFolder::User: return home;
        case SpecialFolder::Desktop: return Combine(home, "Desktop");
        case SpecialFolder::Documents: return Combine(home, "Documents");
        case SpecialFolder::Downloads: return Combine(home, "Downloads");
        case SpecialFolder::Music: return Combine(home, "Music");
        case SpecialFolder::Pictures: return Combine(home, "Pictures");
        case SpecialFolder::Videos: return Combine(home, "Videos");
        case SpecialFolder::AppDataRoaming:
        case SpecialFolder::LocalAppData: {
            const char* xdg = std::getenv("XDG_CONFIG_HOME");
            return (xdg && *xdg) ? std::string(xdg) : Combine(home, ".config");
        }
        case SpecialFolder::ProgramData:
            return "/usr/share";
        default:
            IDX_THROW(IndexErrorCode::InvalidArgument, "Unknown SpecialFolder");
        }
#endif
    }

    std::string Path::ExecutableDir()
    {
#ifdef IDX_PLATFORM_WINDOWS
        // Grow the buffer until the path fits — silent truncation at MAX_PATH would
        // produce a wrong directory on long-installation paths (the registry-key value
        // is `LongPathsEnabled = 1` in modern Windows, paths of 1000+ chars are real).
        std::vector<wchar_t> buf(MAX_PATH);
        for (;;) {
            DWORD copied = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (copied == 0) {
                IDX_THROW(IndexErrorCode::Undefined, "GetModuleFileNameW failed");
            }
            if (copied < buf.size()) break; // fit
            // Buffer was too small — Windows null-terminates and returns size on truncation.
            buf.resize(buf.size() * 2);
            if (buf.size() > 65536) {
                IDX_THROW(IndexErrorCode::Undefined, "Executable path > 64KB; refusing");
            }
        }
        return std::filesystem::path(buf.data()).parent_path().string();
#else
        char buf[PATH_MAX]{};
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len <= 0)
            IDX_THROW(IndexErrorCode::Undefined, "Failed to resolve executable path");
        buf[len] = '\0';
        return std::filesystem::path(buf).parent_path().string();
#endif
    }

    std::string Path::ResolveIndexAssets(const std::string& subdirectory) {
        std::string exeDir = ExecutableDir();

        // 1. Packaged build: IndexAssets next to the executable
        std::string packed = Combine(exeDir, "IndexAssets", subdirectory);
        if (std::filesystem::exists(packed))
            return packed;

        // 2. Dev layout: shared IndexAssets one level up (bin/<config>/IndexAssets/)
        std::string dev = Combine(exeDir, "..", "IndexAssets", subdirectory);
        if (std::filesystem::exists(dev))
            return std::filesystem::canonical(dev).string();

        return {};
    }

    std::string Path::ToProjectRelativeDisplay(const std::string& absolutePath) {
        if (absolutePath.empty()) return {};

        std::error_code ec;
        std::filesystem::path input(absolutePath);
        std::filesystem::path canonInput = std::filesystem::weakly_canonical(input, ec);
        if (ec || canonInput.empty()) canonInput = input;

        auto tryRelative = [&canonInput](const std::string& rootStr,
                                         const std::string& displayPrefix) -> std::optional<std::string> {
            if (rootStr.empty()) return std::nullopt;
            std::error_code rec;
            std::filesystem::path canonRoot = std::filesystem::weakly_canonical(
                std::filesystem::path(rootStr), rec);
            if (rec || canonRoot.empty()) canonRoot = std::filesystem::path(rootStr);

            std::filesystem::path rel = canonInput.lexically_relative(canonRoot);
            const std::string relString = rel.generic_string();
            // lexically_relative returns "." for equality, "" when the roots
            // differ (e.g. C: vs D:), and a "..-prefix" when the input is
            // outside the root. None of those are valid display strings.
            if (relString.empty() || relString == ".") return std::nullopt;
            if (relString.rfind("..", 0) == 0) return std::nullopt;
            if (displayPrefix.empty()) return relString;
            return displayPrefix + "/" + relString;
        };

        if (IndexProject* project = ProjectManager::GetCurrentProject()) {
            if (auto rel = tryRelative(project->RootDirectory, {}); rel) {
                return *rel;
            }
        }

        // Engine-shipped IndexAssets (e.g. the default font when no project is
        // loaded, or the editor itself walking the engine assets directory).
        const std::string engineIndexAssets = ResolveIndexAssets("");
        if (!engineIndexAssets.empty()) {
            if (auto rel = tryRelative(engineIndexAssets, "IndexAssets"); rel) {
                return *rel;
            }
        }

        // Last-resort fallback: surface just the filename so we never leak the
        // full machine-local path even for unexpected inputs.
        return std::filesystem::path(absolutePath).filename().string();
    }
}