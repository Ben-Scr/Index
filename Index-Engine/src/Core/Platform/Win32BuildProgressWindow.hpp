#pragma once
#include "Core/Export.hpp"
#include <string>

// Windows-only progress popup (real OS window, not ImGui overlay). No-ops on non-Windows.
namespace Index::Win32BuildProgressWindow {

#ifdef IDX_PLATFORM_WINDOWS

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
