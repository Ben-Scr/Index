#pragma once

#include "Core/Export.hpp"

#include <string>

namespace Index {

	// Installs a process-wide crash handler that writes a minidump to
	// {LocalAppData}/Index/CrashDumps/ on Windows so users have
	// something to send when a shipped build dies. On Linux it
	// installs signal handlers that flush the log and write a short
	// stderr banner before re-raising; no minidump (would require an
	// extra dependency such as breakpad).
	//
	// Initialize() is idempotent — InitializeCore() calls it once for
	// the whole process after Log::Initialize so the handler can flush
	// spdlog before the dump fires.
	class INDEX_API CrashReporter {
	public:
		static void Initialize();
		static void Shutdown();

		// Directory the next dump will be written to. Created lazily by
		// Initialize. Useful for the editor to show a "Recent Crashes"
		// link, or for tests to assert location.
		static const std::string& GetDumpDirectory();
	};

} // namespace Index
