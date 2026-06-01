#pragma once

namespace Index {
	class Window;
}

namespace Index {

	// Ref-counted ImGui context owner for the runtime exe; first Acquire creates the context, last Release destroys it.
	class RuntimeImGuiHost {
	public:
		static bool Acquire(Window* window);
		static void Release();
		static bool IsInitialized();
		// BeginFrame/EndFrame are idempotent: only the first call per frame opens NewFrame and the last closes it.
		static void BeginFrame();
		static void EndFrame();
	};

} // namespace Index
