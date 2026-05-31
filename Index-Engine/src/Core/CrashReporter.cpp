#include "pch.hpp"

#include "Core/CrashReporter.hpp"
#include "Core/Log.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/SpecialFolder.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <mutex>

#ifdef IDX_PLATFORM_WINDOWS
#  include <windows.h>
#  include <dbghelp.h>
#  pragma comment(lib, "dbghelp.lib")
#else
#  include <csignal>
#  include <cstdio>
#  include <cstring>
#  include <unistd.h>
#endif

namespace Index {

	namespace {
		std::mutex g_StateMutex;
		bool g_Initialized = false;
		std::string g_DumpDirectory;
		std::string g_AppStem;

#ifdef IDX_PLATFORM_WINDOWS
		LPTOP_LEVEL_EXCEPTION_FILTER g_PreviousFilter = nullptr;
#endif

		// Reentry guard. A crash inside the crash handler must not
		// recursively re-enter — we'd corrupt the dump and likely
		// deadlock on whatever invariant tripped originally.
		std::atomic<bool> g_HandlerEntered{ false };

		std::string ResolveExecutableStem() {
#ifdef IDX_PLATFORM_WINDOWS
			wchar_t buffer[MAX_PATH];
			DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
			if (length == 0 || length == MAX_PATH) {
				return "Index";
			}
			return std::filesystem::path(buffer).stem().string();
#else
			char buffer[4096];
			ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
			if (length <= 0) {
				return "Index";
			}
			buffer[length] = '\0';
			return std::filesystem::path(buffer).stem().string();
#endif
		}

		std::string FormatTimestamp() {
			using std::chrono::system_clock;
			const auto now = system_clock::to_time_t(system_clock::now());
			std::tm tm{};
#ifdef IDX_PLATFORM_WINDOWS
			localtime_s(&tm, &now);
#else
			localtime_r(&now, &tm);
#endif
			char buf[32];
			std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
			return buf;
		}

		std::string ResolveDumpDirectory() {
			try {
				const std::string base = Path::GetSpecialFolderPath(SpecialFolder::LocalAppData);
				if (base.empty()) {
					return {};
				}
				std::filesystem::path dir = std::filesystem::path(base) / "Index" / "CrashDumps";
				std::error_code ec;
				std::filesystem::create_directories(dir, ec);
				if (ec) {
					return {};
				}
				return dir.string();
			}
			catch (...) {
				return {};
			}
		}

#ifdef IDX_PLATFORM_WINDOWS
		LONG CALLBACK ExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
			// Single-shot: only the first crashing thread writes a dump.
			// A second thread crashing while we're mid-write would corrupt
			// the file; better to let it pass through to the OS handler.
			bool expected = false;
			if (!g_HandlerEntered.compare_exchange_strong(expected, true)) {
				return g_PreviousFilter ? g_PreviousFilter(exceptionInfo) : EXCEPTION_CONTINUE_SEARCH;
			}

			std::string dumpPath;
			HANDLE file = INVALID_HANDLE_VALUE;
			bool wrote = false;

			try {
				if (g_DumpDirectory.empty()) {
					g_DumpDirectory = ResolveDumpDirectory();
				}
				if (!g_DumpDirectory.empty()) {
					dumpPath = (std::filesystem::path(g_DumpDirectory)
						/ (g_AppStem + "_" + FormatTimestamp() + ".dmp")).string();

					file = CreateFileA(
						dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
						CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

					if (file != INVALID_HANDLE_VALUE) {
						MINIDUMP_EXCEPTION_INFORMATION mei{};
						mei.ThreadId = GetCurrentThreadId();
						mei.ExceptionPointers = exceptionInfo;
						mei.ClientPointers = FALSE;

						const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
							MiniDumpWithDataSegs |
							MiniDumpWithIndirectlyReferencedMemory |
							MiniDumpScanMemory |
							MiniDumpWithThreadInfo);

						wrote = MiniDumpWriteDump(
							GetCurrentProcess(), GetCurrentProcessId(),
							file, type, &mei, nullptr, nullptr) != FALSE;

						CloseHandle(file);
					}
				}
			}
			catch (...) {
				// Anything thrown inside the handler is swallowed; we
				// still want the OS / previous filter to run.
			}

			// Best-effort log flush. If logging itself is what broke,
			// this will throw — which is fine, we catch and move on.
			try {
				if (wrote && !dumpPath.empty()) {
					IDX_CORE_FATAL_TAG("CrashReporter", "Wrote minidump to {}", dumpPath);
				}
				else {
					IDX_CORE_FATAL_TAG("CrashReporter",
						"Crash detected but minidump could not be written (dir='{}')",
						g_DumpDirectory);
				}
				if (auto core = Log::GetCoreLogger()) {
					core->flush();
				}
				if (auto client = Log::GetClientLogger()) {
					client->flush();
				}
			}
			catch (...) {}

			// Let the OS / previous filter take over so Windows still
			// gets to record its own dump in %LOCALAPPDATA%\CrashDumps
			// if WER is configured, and the process dies normally.
			return g_PreviousFilter ? g_PreviousFilter(exceptionInfo) : EXCEPTION_CONTINUE_SEARCH;
		}
#else
		void SignalHandler(int signal) {
			bool expected = false;
			if (!g_HandlerEntered.compare_exchange_strong(expected, true)) {
				// Re-entry: restore default disposition and re-raise.
				std::signal(signal, SIG_DFL);
				std::raise(signal);
				return;
			}

			const char* name = "signal";
			switch (signal) {
			case SIGSEGV: name = "SIGSEGV"; break;
			case SIGABRT: name = "SIGABRT"; break;
			case SIGFPE:  name = "SIGFPE";  break;
			case SIGILL:  name = "SIGILL";  break;
			case SIGBUS:  name = "SIGBUS";  break;
			default: break;
			}

			// Async-signal-safe-ish: write() on stderr is the only thing
			// we strictly need to communicate the crash. Everything else
			// (spdlog flush) is best-effort.
			const char prefix[] = "[Index] fatal signal: ";
			(void)::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
			(void)::write(STDERR_FILENO, name, std::strlen(name));
			(void)::write(STDERR_FILENO, "\n", 1);

			try {
				if (auto core = Log::GetCoreLogger()) core->flush();
				if (auto client = Log::GetClientLogger()) client->flush();
			}
			catch (...) {}

			// Restore default and re-raise so a debugger / coredump
			// pipeline can still catch it.
			std::signal(signal, SIG_DFL);
			std::raise(signal);
		}
#endif
	} // namespace

	void CrashReporter::Initialize() {
		std::scoped_lock lock(g_StateMutex);
		if (g_Initialized) {
			return;
		}

		g_AppStem = ResolveExecutableStem();
		g_DumpDirectory = ResolveDumpDirectory();

#ifdef IDX_PLATFORM_WINDOWS
		g_PreviousFilter = SetUnhandledExceptionFilter(&ExceptionFilter);
#else
		std::signal(SIGSEGV, &SignalHandler);
		std::signal(SIGABRT, &SignalHandler);
		std::signal(SIGFPE,  &SignalHandler);
		std::signal(SIGILL,  &SignalHandler);
		std::signal(SIGBUS,  &SignalHandler);
#endif

		g_Initialized = true;

		if (!g_DumpDirectory.empty()) {
			IDX_CORE_INFO_TAG("CrashReporter", "Crash dumps will be written to {}", g_DumpDirectory);
		}
		else {
			IDX_CORE_WARN_TAG("CrashReporter",
				"Could not resolve LocalAppData/Index/CrashDumps — crashes will not produce a dump file");
		}
	}

	void CrashReporter::Shutdown() {
		std::scoped_lock lock(g_StateMutex);
		if (!g_Initialized) {
			return;
		}

#ifdef IDX_PLATFORM_WINDOWS
		SetUnhandledExceptionFilter(g_PreviousFilter);
		g_PreviousFilter = nullptr;
#else
		std::signal(SIGSEGV, SIG_DFL);
		std::signal(SIGABRT, SIG_DFL);
		std::signal(SIGFPE,  SIG_DFL);
		std::signal(SIGILL,  SIG_DFL);
		std::signal(SIGBUS,  SIG_DFL);
#endif

		g_Initialized = false;
	}

	const std::string& CrashReporter::GetDumpDirectory() {
		std::scoped_lock lock(g_StateMutex);
		return g_DumpDirectory;
	}

} // namespace Index
