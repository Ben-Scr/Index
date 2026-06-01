#pragma once
#include "Core/Export.hpp"

namespace Index {

	// Shares the editor's ImGui context + allocators with package DLLs so each DLL's static GImGui stays in sync.
	// Editor calls Publish after CreateContext; packages call SyncImGui lazily before their first ImGui call.
	class INDEX_API PackageImGuiBridge {
	public:
		static void Publish(void* imguiContext, void* allocFn, void* freeFn, void* userData);

		// Editor-side: call before ImGui::DestroyContext(). Resets the
		// context pointer to nullptr and bumps the generation, so any
		// re-attached editor session can re-publish cleanly.
		static void Clear();

		// Package-side accessors.
		static void* GetContext();
		static void GetAllocators(void*& outAllocFn, void*& outFreeFn, void*& outUserData);
		static bool HasContext();

		static unsigned long long GetGeneration();
	};

} // namespace Index
