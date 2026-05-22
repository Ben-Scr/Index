#pragma once
#include "Collections/Color.hpp"
#include "Core/Export.hpp"

struct GLFWwindow;

namespace Index { class Window; }

// Windows-only custom titlebar layer. Subclasses the GLFW HWND's WndProc
// so the engine can own the entire window decoration — drag, resize,
// Aero Snap, double-click maximize — while GLFW continues to handle
// every other message (keys, mouse, IME, focus). All non-Windows
// platforms get inline no-op stubs so callers stay cross-platform.
namespace Index::Win32Titlebar {

#ifdef IDX_PLATFORM_WINDOWS

    // Installs the WndProc subclass and restores the DWM drop shadow.
    // Idempotent — a second call on the same HWND is logged and ignored.
    // `owner` is stashed in per-HWND state so future phases can route
    // window events (maximize, dpi-change) back to the C++ Window.
    void Install(GLFWwindow* window, Window* owner);

    // Restores the original WndProc and purges the per-HWND state.
    // MUST run before glfwDestroyWindow — otherwise Windows can dispatch
    // a teardown message into our subclass after the state map is gone.
    void Uninstall(GLFWwindow* window);

    // Logical-pixel height of the draggable caption row. The hit-test
    // handler DPI-scales this at query time.
    void SetTitlebarHeight(int logicalPx);
    int  GetTitlebarHeight();
    // Physical-pixel height for the given window (logical * DPI / 96).
    // Returns the logical value if the HWND can't be queried.
    int  GetTitlebarHeightPhysical(GLFWwindow* window);

    // Publishes the draggable caption rectangle in window-local pixels
    // (origin top-left). Replaces the previous frame's value. Setting
    // w=0 or h=0 disables the caption hit region entirely.
    void SetCaptionRect(int x, int y, int w, int h);
    // Registers a non-client override rectangle (window-local pixels).
    // Points inside any non-client rect that would otherwise be HTCAPTION
    // get HTCLIENT instead — so ImGui buttons drawn inside the caption
    // row receive clicks instead of initiating a window drag.
    void AddNonClientRect(int x, int y, int w, int h);
    // Clears the non-client override list. The titlebar layer calls
    // this once per frame before re-publishing the current frame's
    // button + menu-item rects.
    void ResetNonClientRects();

    // Toggles DWMWA_USE_IMMERSIVE_DARK_MODE on the given window's HWND
    // (attribute 20, Win10 1809+). Silently no-ops on older Windows
    // builds. Has a visible effect on the OS titlebar only when the
    // window keeps native chrome — once the custom titlebar is drawn
    // over it, this only affects DWM-rendered borders / shadows.
    void SetDarkMode(GLFWwindow* window, bool enabled);
    // Applies native Windows caption/text colors via DWM where supported.
    // Alpha <= 0 restores the system default for that slot.
    void SetColors(GLFWwindow* window, Color caption, Color text, Color border);

#else

    inline void Install(GLFWwindow*, Window*) {}
    inline void Uninstall(GLFWwindow*) {}
    inline void SetTitlebarHeight(int) {}
    inline int  GetTitlebarHeight() { return 0; }
    inline int  GetTitlebarHeightPhysical(GLFWwindow*) { return 0; }
    inline void SetCaptionRect(int, int, int, int) {}
    inline void AddNonClientRect(int, int, int, int) {}
    inline void ResetNonClientRects() {}
    inline void SetDarkMode(GLFWwindow*, bool) {}
    inline void SetColors(GLFWwindow*, Color, Color, Color) {}

#endif

}
