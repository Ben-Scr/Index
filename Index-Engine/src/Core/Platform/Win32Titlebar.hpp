#pragma once
#include "Collections/Color.hpp"
#include "Core/Export.hpp"

struct GLFWwindow;

namespace Index { class Window; }

namespace Index::Win32Titlebar {

#ifdef IDX_PLATFORM_WINDOWS

    void Install(GLFWwindow* window, Window* owner);
    // MUST run before glfwDestroyWindow — Windows can dispatch teardown messages into the subclass after state is gone.
    void Uninstall(GLFWwindow* window);

    // Logical-pixel height of the draggable caption row. The hit-test
    // handler DPI-scales this at query time.
    void SetTitlebarHeight(int logicalPx);
    int  GetTitlebarHeight();
    // Physical-pixel height for the given window (logical * DPI / 96).
    // Returns the logical value if the HWND can't be queried.
    int  GetTitlebarHeightPhysical(GLFWwindow* window);

    void SetCaptionRect(int x, int y, int w, int h);
    // Points inside non-client rects that would be HTCAPTION get HTCLIENT — ImGui buttons in the caption row receive clicks.
    void AddNonClientRect(int x, int y, int w, int h);
    void ResetNonClientRects();

    void SetDarkMode(GLFWwindow* window, bool enabled);
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
