#include "pch.hpp"
#include "Win32Titlebar.hpp"

#ifdef IDX_PLATFORM_WINDOWS

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>

namespace Index::Win32Titlebar {

    namespace {

        struct State {
            WNDPROC OriginalWndProc = nullptr;
            Window* Owner = nullptr;
        };

        std::unordered_map<HWND, State>& StateMap() {
            static std::unordered_map<HWND, State> s_Map;
            return s_Map;
        }

        int g_TitlebarHeightLogical = 32;

        struct Rect {
            int X = 0, Y = 0, W = 0, H = 0;
            bool Contains(int px, int py) const {
                return px >= X && px < X + W && py >= Y && py < Y + H;
            }
            bool Empty() const { return W <= 0 || H <= 0; }
        };

        Rect g_CaptionRect{};
        std::vector<Rect> g_NonClientRects;

        int DpiScale(int logical, UINT dpi) {
            return MulDiv(logical, static_cast<int>(dpi), 96);
        }

        COLORREF ToColorRef(Color color) {
            const Color c = color.Clamped();
            const auto toByte = [](float value) -> BYTE {
                return static_cast<BYTE>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            };
            return RGB(toByte(c.r), toByte(c.g), toByte(c.b));
        }

        void SetColorAttribute(HWND hwnd, DWORD attribute, Color color) {
            constexpr COLORREF k_DwmDefaultColor = 0xFFFFFFFF;
            COLORREF value = color.a > 0.0f ? ToColorRef(color) : k_DwmDefaultColor;
            DwmSetWindowAttribute(hwnd, attribute, &value, sizeof(value));
        }

        LRESULT CALLBACK TitlebarWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
            auto& map = StateMap();
            auto it = map.find(hwnd);
            if (it == map.end()) {
                // State already torn down (Uninstall raced with a queued
                // message). Defer to the default handler — better than a
                // null-deref.
                return DefWindowProcW(hwnd, msg, wparam, lparam);
            }
            WNDPROC originalProc = it->second.OriginalWndProc;

            switch (msg) {
            case WM_NCCALCSIZE: {
                if (wparam == TRUE) {
                    // Maximized: inset by frame+padding to keep window inside the monitor work area.
                    if (IsZoomed(hwnd)) {
                        NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                        const UINT dpi = GetDpiForWindow(hwnd);
                        const int frameX = GetSystemMetricsForDpi(SM_CXFRAME, dpi);
                        const int frameY = GetSystemMetricsForDpi(SM_CYFRAME, dpi);
                        const int padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                        params->rgrc[0].left   += frameX + padding;
                        params->rgrc[0].top    += frameY + padding;
                        params->rgrc[0].right  -= frameX + padding;
                        params->rgrc[0].bottom -= frameY + padding;
                    }
                    return 0;
                }
                break;
            }
            case WM_NCHITTEST: {
                const POINT cursor{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                RECT window;
                GetWindowRect(hwnd, &window);
                const int x = cursor.x - window.left;
                const int y = cursor.y - window.top;
                const int w = window.right - window.left;
                const int h = window.bottom - window.top;
                const UINT dpi = GetDpiForWindow(hwnd);
                const int border = MulDiv(8, static_cast<int>(dpi), 96);
                const bool resizable = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) != 0;
                const bool maximized = IsZoomed(hwnd) != FALSE;

                if (resizable && !maximized) {
                    const bool onLeft   = x < border;
                    const bool onRight  = x >= w - border;
                    const bool onTop    = y < border;
                    const bool onBottom = y >= h - border;
                    if (onTop    && onLeft)  return HTTOPLEFT;
                    if (onTop    && onRight) return HTTOPRIGHT;
                    if (onBottom && onLeft)  return HTBOTTOMLEFT;
                    if (onBottom && onRight) return HTBOTTOMRIGHT;
                    if (onLeft)   return HTLEFT;
                    if (onRight)  return HTRIGHT;
                    if (onTop)    return HTTOP;
                    if (onBottom) return HTBOTTOM;
                }

                if (!g_CaptionRect.Empty() && g_CaptionRect.Contains(x, y)) {
                    for (const auto& r : g_NonClientRects) {
                        if (r.Contains(x, y)) {
                            return HTCLIENT;
                        }
                    }
                    return HTCAPTION;
                }
                return HTCLIENT;
            }
            case WM_GETMINMAXINFO: {
                // Without WS_CAPTION the default sizing covers the full monitor; constrain to rcWork so maximize respects the taskbar.
                HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(monitor, &mi)) {
                    MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
                    mmi->ptMaxPosition.x = mi.rcWork.left   - mi.rcMonitor.left;
                    mmi->ptMaxPosition.y = mi.rcWork.top    - mi.rcMonitor.top;
                    mmi->ptMaxSize.x     = mi.rcWork.right  - mi.rcWork.left;
                    mmi->ptMaxSize.y     = mi.rcWork.bottom - mi.rcWork.top;
                }
                return 0;
            }
            case WM_NCPAINT:
                // We own all painting — never let the default frame paint.
                return 0;
            case WM_NCACTIVATE:
                // Returning TRUE suppresses the inactive-frame repaint
                // flicker that DWM otherwise does on focus change.
                return TRUE;
            case WM_DPICHANGED: {
                const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
                if (suggested) {
                    SetWindowPos(hwnd, nullptr,
                        suggested->left, suggested->top,
                        suggested->right - suggested->left,
                        suggested->bottom - suggested->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                return 0;
            }
            case WM_SIZE: {
                // Re-apply DWM margins on restore; some Windows builds drop them across minimize/restore.
                if (wparam == SIZE_RESTORED) {
                    MARGINS m{ 0, 0, 0, 1 };
                    DwmExtendFrameIntoClientArea(hwnd, &m);
                }
                break;
            }
            default:
                break;
            }

            return CallWindowProcW(originalProc, hwnd, msg, wparam, lparam);
        }

    }

    void Install(GLFWwindow* window, Window* owner) {
        if (!window) return;
        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) return;

        auto& map = StateMap();
        if (map.find(hwnd) != map.end()) {
            IDX_CORE_WARN_TAG("Win32Titlebar", "Install called twice for HWND {}", static_cast<void*>(hwnd));
            return;
        }

        State state;
        state.Owner = owner;
        state.OriginalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&TitlebarWndProc)));
        if (!state.OriginalWndProc) {
            IDX_CORE_ERROR_TAG("Win32Titlebar",
                "SetWindowLongPtrW failed (GetLastError={})", GetLastError());
            return;
        }
        map.emplace(hwnd, state);

        MARGINS margins{ 0, 0, 0, 1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);

        // SWP_FRAMECHANGED forces WM_NCCALCSIZE immediately; without it the NC strip is deferred to the next user resize.
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void Uninstall(GLFWwindow* window) {
        if (!window) return;
        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) return;

        auto& map = StateMap();
        auto it = map.find(hwnd);
        if (it == map.end()) return;

        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(it->second.OriginalWndProc));
        map.erase(it);
    }

    void SetTitlebarHeight(int logicalPx) {
        g_TitlebarHeightLogical = logicalPx;
    }

    int GetTitlebarHeight() {
        return g_TitlebarHeightLogical;
    }

    int GetTitlebarHeightPhysical(GLFWwindow* window) {
        if (!window) return g_TitlebarHeightLogical;
        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) return g_TitlebarHeightLogical;
        const UINT dpi = GetDpiForWindow(hwnd);
        return DpiScale(g_TitlebarHeightLogical, dpi);
    }

    void SetCaptionRect(int x, int y, int w, int h) {
        g_CaptionRect = Rect{ x, y, w, h };
    }

    void AddNonClientRect(int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) return;
        g_NonClientRects.push_back(Rect{ x, y, w, h });
    }

    void ResetNonClientRects() {
        g_NonClientRects.clear();
    }

    void SetDarkMode(GLFWwindow* window, bool enabled) {
        if (!window) return;
        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) return;
        constexpr DWORD k_DwmwaUseImmersiveDarkMode = 20;
        BOOL useDark = enabled ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, k_DwmwaUseImmersiveDarkMode, &useDark, sizeof(useDark));
    }

    void SetColors(GLFWwindow* window, Color caption, Color text, Color border) {
        if (!window) return;
        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) return;

        constexpr DWORD k_DwmwaBorderColor = 34;
        constexpr DWORD k_DwmwaCaptionColor = 35;
        constexpr DWORD k_DwmwaTextColor = 36;
        SetColorAttribute(hwnd, k_DwmwaCaptionColor, caption);
        SetColorAttribute(hwnd, k_DwmwaTextColor, text);
        SetColorAttribute(hwnd, k_DwmwaBorderColor, border);
    }

}

#endif
