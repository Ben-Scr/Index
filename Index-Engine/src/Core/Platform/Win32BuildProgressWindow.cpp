#include "pch.hpp"
#include "Win32BuildProgressWindow.hpp"

#ifdef IDX_PLATFORM_WINDOWS

#include "Core/Window.hpp"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <dwmapi.h>
#include <algorithm>

namespace Index::Win32BuildProgressWindow {

    namespace {

        // Logical (96-DPI) dimensions. The WndProc DPI-scales these per
        // monitor so the popup stays crisp on hi-DPI displays.
        constexpr int k_LogicalWidth  = 360;
        constexpr int k_LogicalHeight = 110;

        constexpr COLORREF k_ColBackground = RGB(28, 28, 28);
        constexpr COLORREF k_ColBorder     = RGB(60, 60, 60);
        constexpr COLORREF k_ColTitle      = RGB(235, 235, 235);
        constexpr COLORREF k_ColStage      = RGB(150, 150, 150);
        constexpr COLORREF k_ColBarTrack   = RGB(45, 45, 45);
        constexpr COLORREF k_ColBarFill    = RGB(232, 188, 32); // amber, matches editor theme accent

        const wchar_t* const k_ClassName = L"Index.BuildProgressWindow";

        struct State {
            HWND Hwnd = nullptr;
            ATOM ClassAtom = 0;
            float Progress = 0.0f;
            std::wstring Stage;
            std::wstring Title; // always set by Show() before the window appears
        };

        State& Get() {
            static State s_State;
            return s_State;
        }

        UINT QueryDpi(HWND hwnd) {
            // GetDpiForWindow is Win10 1607+. Fall back to system DPI on
            // older Windows so the popup still renders at a reasonable
            // size instead of dividing by zero.
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            if (user32) {
                using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
                auto fn = reinterpret_cast<GetDpiForWindowFn>(
                    GetProcAddress(user32, "GetDpiForWindow"));
                if (fn) return fn(hwnd);
            }
            HDC hdc = GetDC(nullptr);
            UINT dpi = hdc ? static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX)) : 96u;
            if (hdc) ReleaseDC(nullptr, hdc);
            return dpi == 0 ? 96u : dpi;
        }

        int Scale(int logical, UINT dpi) {
            return MulDiv(logical, static_cast<int>(dpi), 96);
        }

        std::wstring Widen(const std::string& s) {
            if (s.empty()) return {};
            const int needed = MultiByteToWideChar(CP_UTF8, 0,
                s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                out.data(), needed);
            return out;
        }

        void PaintWindow(HWND hwnd) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (!hdc) return;

            RECT client;
            GetClientRect(hwnd, &client);
            const int w = client.right - client.left;
            const int h = client.bottom - client.top;
            const UINT dpi = QueryDpi(hwnd);

            // Double-buffer through a memory DC so the WM_ERASEBKGND skip
            // doesn't flash the previous frame's content between fills.
            HDC memDc = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

            // Background fill + 1px border.
            HBRUSH bgBrush = CreateSolidBrush(k_ColBackground);
            FillRect(memDc, &client, bgBrush);
            DeleteObject(bgBrush);

            HBRUSH borderBrush = CreateSolidBrush(k_ColBorder);
            FrameRect(memDc, &client, borderBrush);
            DeleteObject(borderBrush);

            SetBkMode(memDc, TRANSPARENT);

            const int padX     = Scale(16, dpi);
            const int padTop   = Scale(14, dpi);
            const int gap      = Scale(8,  dpi);
            const int barH     = Scale(8,  dpi);
            const int titleH   = Scale(18, dpi);
            const int stageH   = Scale(16, dpi);
            const int titleFs  = Scale(14, dpi);
            const int stageFs  = Scale(12, dpi);

            // Title text ("Building Project...").
            HFONT titleFont = CreateFontW(
                -titleFs, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HGDIOBJ oldFont = SelectObject(memDc, titleFont);
            SetTextColor(memDc, k_ColTitle);
            RECT titleRect{ padX, padTop, w - padX, padTop + titleH };
            const State& st = Get();
            DrawTextW(memDc, st.Title.c_str(), -1, &titleRect,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(memDc, oldFont);
            DeleteObject(titleFont);

            // Progress-bar track + fill.
            const int barY = padTop + titleH + gap;
            RECT barTrack{ padX, barY, w - padX, barY + barH };
            HBRUSH trackBrush = CreateSolidBrush(k_ColBarTrack);
            FillRect(memDc, &barTrack, trackBrush);
            DeleteObject(trackBrush);

            const float progress = std::clamp(st.Progress, 0.0f, 1.0f);
            const int   fillWidth = static_cast<int>((barTrack.right - barTrack.left) * progress);
            if (fillWidth > 0) {
                RECT barFill{ padX, barY, padX + fillWidth, barY + barH };
                HBRUSH fillBrush = CreateSolidBrush(k_ColBarFill);
                FillRect(memDc, &barFill, fillBrush);
                DeleteObject(fillBrush);
            }

            // Stage text under the bar.
            HFONT stageFont = CreateFontW(
                -stageFs, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            oldFont = SelectObject(memDc, stageFont);
            SetTextColor(memDc, k_ColStage);
            RECT stageRect{ padX, barY + barH + gap, w - padX,
                            barY + barH + gap + stageH };
            const wchar_t* stageText = st.Stage.empty() ? L"Working..." : st.Stage.c_str();
            DrawTextW(memDc, stageText, -1, &stageRect,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(memDc, oldFont);
            DeleteObject(stageFont);

            BitBlt(hdc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);

            SelectObject(memDc, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDc);
            EndPaint(hwnd, &ps);
        }

        LRESULT CALLBACK BuildProgressWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
            switch (msg) {
                case WM_ERASEBKGND:
                    // Custom WM_PAINT paints every pixel — suppress the
                    // default erase to avoid flicker between frames.
                    return 1;
                case WM_PAINT:
                    PaintWindow(hwnd);
                    return 0;
                case WM_CLOSE:
                    // Ignore Alt+F4 / system-menu close — the build worker
                    // owns the popup lifetime; users can't cancel a build
                    // by closing the progress window. Hide() will tear it
                    // down when the worker finishes.
                    return 0;
                case WM_DESTROY:
                    Get().Hwnd = nullptr;
                    return 0;
                default:
                    break;
            }
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        bool EnsureClassRegistered() {
            State& st = Get();
            if (st.ClassAtom != 0) return true;
            WNDCLASSEXW wc{};
            wc.cbSize        = sizeof(wc);
            wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            wc.lpfnWndProc   = BuildProgressWndProc;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr; // handled by WM_PAINT
            wc.lpszClassName = k_ClassName;
            st.ClassAtom = RegisterClassExW(&wc);
            return st.ClassAtom != 0;
        }

        void ComputeCenteredRect(int width, int height, int& outX, int& outY) {
            // Prefer the editor's main window monitor so the popup lands on
            // the same screen the user is looking at. Fall back to the
            // primary monitor's work area when the engine window isn't up
            // yet (e.g. command-line build invocation).
            HWND parent = nullptr;
            if (Window* w = Window::GetActiveWindow()) {
                if (GLFWwindow* g = w->GetGLFWWindow()) {
                    parent = glfwGetWin32Window(g);
                }
            }
            RECT area{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
            HMONITOR mon = parent
                ? MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST)
                : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(mon, &mi)) {
                area = mi.rcWork;
            }
            outX = area.left + ((area.right - area.left) - width) / 2;
            outY = area.top  + ((area.bottom - area.top) - height) / 2;
        }

    } // namespace

    void Show(const std::string& title) {
        State& st = Get();
        std::wstring wtitle = Widen(title);
        const bool titleChanged = wtitle != st.Title;
        st.Title = std::move(wtitle);

        if (st.Hwnd) {
            // Already shown — just retitle and re-invalidate if the caller
            // switched contexts (e.g. build popup → script recompile).
            if (titleChanged) {
                SetWindowTextW(st.Hwnd, st.Title.c_str());
                InvalidateRect(st.Hwnd, nullptr, FALSE);
            }
            ShowWindow(st.Hwnd, SW_SHOWNA);
            return;
        }
        if (!EnsureClassRegistered()) {
            return;
        }

        // Compute size first using the primary monitor's DPI; once the
        // HWND exists we re-query its actual monitor's DPI and resize if
        // needed (cross-monitor DPI mismatch is rare here since the popup
        // is centered on the editor's monitor below).
        UINT seedDpi = QueryDpi(nullptr);
        int width  = Scale(k_LogicalWidth,  seedDpi);
        int height = Scale(k_LogicalHeight, seedDpi);
        int x, y;
        ComputeCenteredRect(width, height, x, y);

        HWND parent = nullptr;
        if (Window* w = Window::GetActiveWindow()) {
            if (GLFWwindow* g = w->GetGLFWWindow()) {
                parent = glfwGetWin32Window(g);
            }
        }

        // WS_POPUP for a captionless window; WS_EX_TOPMOST keeps it above
        // the editor without us having to chase focus changes. WS_EX_-
        // TOOLWINDOW excludes it from Alt-Tab + the taskbar — this is a
        // transient progress popup, not a real window the user manages.
        st.Hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            k_ClassName,
            st.Title.c_str(),
            WS_POPUP,
            x, y, width, height,
            parent,                  // owner so the popup hides with the editor
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        if (!st.Hwnd) {
            return;
        }

        // Resize once the real per-monitor DPI is known.
        const UINT realDpi = QueryDpi(st.Hwnd);
        if (realDpi != seedDpi) {
            width  = Scale(k_LogicalWidth,  realDpi);
            height = Scale(k_LogicalHeight, realDpi);
            ComputeCenteredRect(width, height, x, y);
            SetWindowPos(st.Hwnd, HWND_TOPMOST, x, y, width, height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        // Native dark-mode titlebar attribute — no visible titlebar on a
        // WS_POPUP, but DWM still uses it for the drop shadow + border
        // tint so the popup matches the editor's dark theme.
        BOOL dark = TRUE;
        DwmSetWindowAttribute(st.Hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
            &dark, sizeof(dark));

        ShowWindow(st.Hwnd, SW_SHOWNA);
        UpdateWindow(st.Hwnd);
    }

    void Update(float progress, const std::string& stage) {
        State& st = Get();
        if (!st.Hwnd) return;

        std::wstring newStage = Widen(stage);
        const float clamped = std::clamp(progress, 0.0f, 1.0f);
        const bool changed = std::abs(clamped - st.Progress) > 0.0005f
            || newStage != st.Stage;
        if (!changed) return;

        st.Progress = clamped;
        st.Stage = std::move(newStage);
        InvalidateRect(st.Hwnd, nullptr, FALSE);
        // Force the WM_PAINT to dispatch this frame so the bar advances
        // visibly without waiting for the next message-loop tick (the
        // editor's glfwPollEvents would deliver it eventually, but
        // UpdateWindow keeps the popup smoother during long builds).
        UpdateWindow(st.Hwnd);
    }

    void Hide() {
        State& st = Get();
        if (!st.Hwnd) return;
        DestroyWindow(st.Hwnd);
        st.Hwnd = nullptr;
        st.Progress = 0.0f;
        st.Stage.clear();
    }

    bool IsVisible() {
        return Get().Hwnd != nullptr;
    }

}

#endif // IDX_PLATFORM_WINDOWS
