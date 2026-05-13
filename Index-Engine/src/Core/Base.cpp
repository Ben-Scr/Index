#include "pch.hpp"
#include "Base.hpp"

#include "Core/Log.hpp"
#include "Core/Memory.hpp"

namespace Index {

	void InitializeCore()
	{
		Allocator::Init();
		Log::Initialize();

		IDX_CORE_TRACE_TAG("Core", "Index Engine {}", IDX_VERSION);
		IDX_CORE_TRACE_TAG("Core", "Initializing...");
	}

	void ShutdownCore()
	{
		IDX_CORE_TRACE_TAG("Core", "Shutting down...");
		Log::Shutdown();
		Allocator::Shutdown();
	}
}
