#include "pch.hpp"
#include "Base.hpp"

#include "Core/CrashReporter.hpp"
#include "Core/Log.hpp"
#include "Core/Memory.hpp"

namespace Index {

	void InitializeCore()
	{
		Allocator::Init();
		Log::Initialize();
		// CrashReporter installed after Log so the handler can flush
		// spdlog before the dump fires.
		CrashReporter::Initialize();

		IDX_CORE_TRACE_TAG("Core", "Index Engine {}", IDX_VERSION);
		IDX_CORE_TRACE_TAG("Core", "Initializing...");
	}

	void ShutdownCore()
	{
		IDX_CORE_TRACE_TAG("Core", "Shutting down...");
		CrashReporter::Shutdown();
		Log::Shutdown();
		Allocator::Shutdown();
	}
}
