#pragma once
#include "Core/Export.hpp"
#include <string>

// Windows-only standalone OS window that shows progress for any
// long-running editor operation (project build, script recompile,
// anything else that needs a "please wait" indicator). The window is
// a real top-level OS window rather than an ImGui overlay so the
// status stays crisp even if the editor's render thread is briefly
// busy. It is pinned in place — users can't drag it — because it is
// a transient progress indicator, not a managed window.
//
// All entry points are no-ops on non-Windows platforms — the editor
// can call them unconditionally without #ifdef guards at the call site.
namespace Index::Win32BuildProgressWindow {

#ifdef IDX_PLATFORM_WINDOWS

    // Creates and shows the popup centered on the editor's main window
    // (or main monitor if the editor window isn't resolvable). Idempotent:
    // calling while the window already exists just re-titles it and
    // brings it forward — safe to invoke every frame.
    INDEX_API void Show(const std::string& title);

    // Pushes the latest progress (0..1) and stage label into the popup.
    // Cheap to call every frame — only invalidates the window if either
    // value actually changed.
    INDEX_API void Update(float progress, const std::string& stage);

    // Destroys the popup. Safe to call when not shown.
    INDEX_API void Hide();

    INDEX_API bool IsVisible();

#else

    inline void Show(const std::string&) {}
    inline void Update(float, const std::string&) {}
    inline void Hide() {}
    inline bool IsVisible() { return false; }

#endif

}
