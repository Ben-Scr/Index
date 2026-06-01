#include "pch.hpp"
#include "Window.hpp"
#include "Application.hpp"
#include "Core/Platform/Win32Titlebar.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/RenderApi.hpp"
#include "Events/WindowEvents.hpp"
#include "Events/KeyEvents.hpp"
#include "Events/MouseEvents.hpp"
#include "Scripting/ScriptEngine.hpp"
#include <Utils/StringHelper.hpp>
#include <algorithm>
#include <cmath>

#ifdef IDX_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace Index {
	Window* Window::s_ActiveWindow = nullptr;
	bool Window::s_IsVsync = true;
	std::unique_ptr<Viewport> Window::s_MainViewport = nullptr;
	Window::UIRegion Window::s_UIRegion{};
	bool Window::s_IsInitialized = false;
	const GLFWvidmode* Window::k_Videomode = nullptr;

	Window::Window(int width, int height, const std::string& title)
	{
		Create(WindowSpecification{ width , height, title });
	}

	Window::Window(const WindowSpecification& props)
	{
		Create(props);
	}

	void Window::RefreshCallback(GLFWwindow* window) {
		(void)window;
		Application* app = Application::GetInstance();
		if (!app) return;
		app->RenderOnceForRefresh();
	}

	void Window::Initialize() {
		if (s_IsInitialized) {
			IDX_CORE_WARN_TAG("Window", "GLFW is already initialized");
			return;
		}

		int code = glfwInit();

		IDX_ASSERT(code == GLFW_TRUE, IndexErrorCode::Undefined, "GLFW library couldn't initialize, error code " + StringHelper::WrapWith(std::to_string(code), '\''));

		// Headless / CI / detached-session: glfwGetPrimaryMonitor can return null.
		GLFWmonitor* mainMonitor = GetMainMonitor();
		if (!mainMonitor) {
			IDX_CORE_ERROR_TAG("Window", "No primary monitor available");
			k_Videomode = nullptr;
			glfwTerminate();
			s_IsInitialized = false;
			return;
		}
		k_Videomode = glfwGetVideoMode(mainMonitor);
		IDX_ASSERT(k_Videomode != nullptr, IndexErrorCode::Undefined, "Failed to query monitor video mode");

		s_IsInitialized = true;
	}

	void Window::Shutdown() {
		if (!s_IsInitialized) {
			return;
		}

		s_MainViewport.reset();
		s_ActiveWindow = nullptr;
		k_Videomode = nullptr;
		// Reset across reloads — last user-set vsync state would otherwise leak.
		s_IsVsync = true;
		s_IsInitialized = false;
		glfwTerminate();
	}

	void Window::SetVsync(bool enabled) {
		// Vsync is owned by the WebGPU swap-chain; see RenderApi::SetVsync.
		// GLFW is initialised with GLFW_NO_API (Dawn owns the GPU context),
		// so glfwSwapInterval has no context to act on here.
		s_IsVsync = enabled;
		RenderApi::SetVsync(enabled);
	}

	void Window::IconifyCallback(GLFWwindow* /*window*/, int iconified) {
		Application::SetWindowMinimized(static_cast<bool>(iconified));
	}

	void Window::FocusCallback(GLFWwindow* window, int focused) {
		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		Application::SetWindowFocused(static_cast<bool>(focused));
		if (win) {
			win->ApplyTitlebarAppearance();
		}

		if ((bool)focused) {
			ScriptEngine::RaiseFocusChanged(true);
			if (win && win->m_EventCallback) {
				WindowFocusEvent e;
				win->m_EventCallback(e);
			}
		}
		else {
			ScriptEngine::RaiseFocusChanged(false);
			if (win && win->m_EventCallback) {
				WindowLostFocusEvent e;
				win->m_EventCallback(e);
			}
		}
	}

	void Window::CloseCallback(GLFWwindow* window) {
		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (win && win->m_EventCallback) {
			WindowCloseEvent e;
			win->m_EventCallback(e);
		}
	}

	void Window::Create(const WindowSpecification& props) {
		IDX_ASSERT(s_IsInitialized, IndexErrorCode::NotInitialized, "The Window isn't initialized");

		m_AspectLock = props.AspectLock > 0.0f ? props.AspectLock : 0.0f;

		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_SAMPLES, 8);

		if (props.CustomTitlebar) {
			glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		}

		s_MainViewport = std::make_unique<Viewport>(
			props.Fullscreen ? k_Videomode->width : props.Width,
			props.Fullscreen ? k_Videomode->height : props.Height
		);

		IDX_INFO_TAG("Window",
			"Create: spec={}x{}, fullscreen={}, videomode={}x{} -> viewport={}x{}",
			props.Width, props.Height, props.Fullscreen,
			k_Videomode ? k_Videomode->width : 0,
			k_Videomode ? k_Videomode->height : 0,
			s_MainViewport->GetWidth(), s_MainViewport->GetHeight());

		m_GLFWwindow = glfwCreateWindow(s_MainViewport->GetWidth(), s_MainViewport->GetHeight(), props.Title.c_str(), nullptr, nullptr);
		IDX_ASSERT(m_GLFWwindow, IndexErrorCode::Undefined, "Failed to create window!");

		m_MinSize = { props.MinWidth, props.MinHeight };
		m_MaxSize = { props.MaxWidth, props.MaxHeight };
		glfwSetWindowSizeLimits(m_GLFWwindow,
			props.MinWidth  > 0 ? props.MinWidth  : GLFW_DONT_CARE,
			props.MinHeight > 0 ? props.MinHeight : GLFW_DONT_CARE,
			props.MaxWidth  > 0 ? props.MaxWidth  : GLFW_DONT_CARE,
			props.MaxHeight > 0 ? props.MaxHeight : GLFW_DONT_CARE);

		SetDecorated(props.Decorated);
		SetResizeable(props.Resizeable);

		m_TitlebarColor         = props.TitlebarColor;
		m_TitlebarTextColor     = props.TitlebarTextColor;
		m_TitlebarActiveColor   = props.TitlebarActiveColor;
		m_TitlebarInactiveColor = props.TitlebarInactiveColor;
		m_TitlebarDarkMode      = props.TitlebarDarkMode;
		if (props.CustomTitlebar) {
			glfwSetWindowAttrib(m_GLFWwindow, GLFW_DECORATED, GLFW_FALSE);
#ifdef IDX_PLATFORM_WINDOWS
			Win32Titlebar::SetTitlebarHeight(props.TitlebarHeight);
			Win32Titlebar::Install(m_GLFWwindow, this);
			ApplyTitlebarAppearance();
#endif
			glfwShowWindow(m_GLFWwindow);
			m_CustomTitlebar = true;
		}
#ifdef IDX_PLATFORM_WINDOWS
		else {
			// Apply native titlebar colors while keeping the OS chrome.
			ApplyTitlebarAppearance();
		}
#endif

		if (props.Fullscreen) {
			// Legacy compat: Fullscreen+Windowed=true maps to Maximized so old projects don't silently switch to Exclusive.
			FullscreenMode mode = props.FullscreenMode;
			if (mode == FullscreenMode::Exclusive && props.Windowed) {
				mode = FullscreenMode::Maximized;
			}
			switch (mode) {
				case FullscreenMode::Exclusive:
					SetFullScreen(true);
					break;
				case FullscreenMode::BorderlessWindowed:
					glfwSetWindowAttrib(m_GLFWwindow, GLFW_DECORATED, GLFW_FALSE);
					if (k_Videomode) {
						glfwSetWindowSize(m_GLFWwindow,
							k_Videomode->width, k_Videomode->height);
						glfwSetWindowPos(m_GLFWwindow, 0, 0);
					}
					break;
				case FullscreenMode::Maximized:
					MaximizeWindow();
					break;
			}
		}
		else {
			CenterWindow();
		}

		{
			int fbW = 0, fbH = 0;
			glfwGetFramebufferSize(m_GLFWwindow, &fbW, &fbH);
			IDX_INFO_TAG("Window",
				"After fullscreen/center: GLFW framebuffer={}x{}", fbW, fbH);
		}

		// No GL context to make current — the render backend handles the device.
		glfwSetWindowUserPointer(m_GLFWwindow, this);

		glfwSetKeyCallback(m_GLFWwindow, SetKeyCallback);
		glfwSetCharCallback(m_GLFWwindow, SetCharCallback);
		glfwSetMouseButtonCallback(m_GLFWwindow, SetMouseButtonCallback);
		glfwSetCursorPosCallback(m_GLFWwindow, SetCursorPositionCallback);
		glfwSetScrollCallback(m_GLFWwindow, SetScrollCallback);
		glfwSetWindowFocusCallback(m_GLFWwindow, FocusCallback);

		glfwSetFramebufferSizeCallback(m_GLFWwindow, SetWindowResizedCallback);
		glfwSetWindowIconifyCallback(m_GLFWwindow, &Window::IconifyCallback);

		glfwSetDropCallback(m_GLFWwindow, SetDropCallback);
		glfwSetWindowRefreshCallback(m_GLFWwindow, RefreshCallback);
		glfwSetWindowCloseCallback(m_GLFWwindow, &Window::CloseCallback);

		// Vsync is handled by the render backend.
		SyncViewportFromFramebuffer();

		s_ActiveWindow = this;
	}
	void Window::Destroy() {
		if (!m_GLFWwindow) {
			return;
		}

		m_Cursor = nullptr;
		m_CursorTextureAssetId = 0;
		if (m_DefaultCursor) {
			glfwDestroyCursor(m_DefaultCursor);
			m_DefaultCursor = nullptr;
		}
		if (m_UICursor) {
			glfwDestroyCursor(m_UICursor);
			m_UICursor = nullptr;
		}
		glfwSetWindowUserPointer(m_GLFWwindow, nullptr);
		m_EventCallback = {};
#ifdef IDX_PLATFORM_WINDOWS
		// MUST uninstall before glfwDestroyWindow: teardown messages can arrive after HWND destruction, dangling the subclass state map entry.
		if (m_CustomTitlebar) {
			Win32Titlebar::Uninstall(m_GLFWwindow);
			m_CustomTitlebar = false;
		}
#endif
		glfwDestroyWindow(m_GLFWwindow);
		m_GLFWwindow = nullptr;
		if (s_ActiveWindow == this) {
			s_ActiveWindow = nullptr;
		}
	}

	void Window::SwapBuffers() const {
		RenderApi::Present();
	}

	void Window::CenterWindow() {
		Vec2Int screenCenter = GetScreenCenter();
		int windowWidth, windowHeight;
		glfwGetWindowSize(m_GLFWwindow, &windowWidth, &windowHeight);
		int posX = screenCenter.x - windowWidth / 2;
		int posY = screenCenter.y - windowHeight / 2;
		glfwSetWindowPos(m_GLFWwindow, posX, posY);
	}
	void Window::Focus() {
		glfwFocusWindow(m_GLFWwindow);
	}
	void Window::MaximizeWindow(bool reset) {
		if (reset && IsMaximized()) {
			glfwRestoreWindow(m_GLFWwindow);
		}
		else {
			glfwMaximizeWindow(m_GLFWwindow);
		}

		SyncViewportFromFramebuffer();
		UpdateViewport();
	}
	void Window::MinimizeWindow() {
		glfwIconifyWindow(m_GLFWwindow);
	}
	void Window::RestoreWindow() {
		glfwRestoreWindow(m_GLFWwindow);
		SyncViewportFromFramebuffer();
		UpdateViewport();
	}

	void Window::SetKeyCallback(GLFWwindow* window, int key, int, int action, int mods) {
		(void)mods;
		if (key == GLFW_KEY_UNKNOWN) {
			return;
		}

		Application* app = Application::GetInstance();
		if (!app) return;

		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));

		switch (action) {
		case GLFW_PRESS:
			app->m_Input.OnKeyDown(key);
			ScriptEngine::RaiseKeyDown(key);
			if (win && win->m_EventCallback) {
				KeyPressedEvent e(key, false);
				win->m_EventCallback(e);
			}
			break;
		case GLFW_RELEASE:
			app->m_Input.OnKeyUp(key);
			ScriptEngine::RaiseKeyUp(key);
			if (win && win->m_EventCallback) {
				KeyReleasedEvent e(key);
				win->m_EventCallback(e);
			}
			break;
		case GLFW_REPEAT:
			app->m_Input.OnKeyDown(key);
			if (win && win->m_EventCallback) {
				KeyPressedEvent e(key, true);
				win->m_EventCallback(e);
			}
			break;
		default:
			break;
		}
	}

	void Window::SetCharCallback(GLFWwindow* /*window*/, unsigned int codepoint) {
		Application* app = Application::GetInstance();
		if (!app) return;
		app->m_Input.OnChar(static_cast<uint32_t>(codepoint));
		ScriptEngine::RaiseEnterChar(static_cast<uint32_t>(codepoint));
	}

	void Window::SetMouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
		(void)mods;
		Application* app = Application::GetInstance();
		if (!app) return;

		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));

		switch (action) {
		case GLFW_PRESS:
			app->m_Input.OnMouseDown(button);
			ScriptEngine::RaiseMouseDown(button);
			if (win && win->m_EventCallback) {
				MouseButtonPressedEvent e(button);
				win->m_EventCallback(e);
			}
			break;
		case GLFW_RELEASE:
			app->m_Input.OnMouseUp(button);
			ScriptEngine::RaiseMouseUp(button);
			if (win && win->m_EventCallback) {
				MouseButtonReleasedEvent e(button);
				win->m_EventCallback(e);
			}
			break;
		default:
			break;
		}
	}

	void Window::SetCursorPositionCallback(GLFWwindow* window, double xPos, double yPos) {
		Application* app = Application::GetInstance();
		if (!app) return;
		app->m_Input.OnMouseMove(xPos, yPos);
		ScriptEngine::RaiseMouseMove(static_cast<float>(xPos), static_cast<float>(yPos));

		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (win && win->m_EventCallback) {
			MouseMovedEvent e(static_cast<float>(xPos), static_cast<float>(yPos));
			win->m_EventCallback(e);
		}
	}

	void Window::SetScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
		Application* app = Application::GetInstance();
		if (!app) return;
		app->m_Input.OnScroll(static_cast<float>(yoffset));
		ScriptEngine::RaiseMouseScroll(static_cast<float>(yoffset));

		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (win && win->m_EventCallback) {
			MouseScrolledEvent e(static_cast<float>(xoffset), static_cast<float>(yoffset));
			win->m_EventCallback(e);
		}
	}

	void Window::SetFullScreen(bool enabled) {
		bool isFullScreen = IsFullScreen();

		if ((isFullScreen && enabled) || (!isFullScreen && !enabled))
			return;

		if (enabled) {
			m_RestoreSize = GetSize();
			m_RestorePos = GetPosition();
		}

		glfwSetWindowMonitor(
			m_GLFWwindow,
			enabled ? GetMainMonitor() : nullptr,
			enabled ? 0 : m_RestorePos.x,
			enabled ? 0 : m_RestorePos.y,
			enabled ? k_Videomode->width : m_RestoreSize.x,
			enabled ? k_Videomode->height : m_RestoreSize.y,
			k_Videomode->refreshRate
		);
		SyncViewportFromFramebuffer();
		UpdateViewport();

		if (!enabled) {
			SetSize(m_RestoreSize);
			SetPosition(m_RestorePos);
		}
	}

	void Window::SetDecorated(bool enabled) {
		glfwSetWindowAttrib(m_GLFWwindow, GLFW_DECORATED, enabled ? GLFW_TRUE : GLFW_FALSE);
	}
	void Window::SetVisible(bool enabled) {
		if (enabled)
			glfwShowWindow(m_GLFWwindow);
		else
			glfwHideWindow(m_GLFWwindow);
	}

	void Window::SetResizeable(bool enabled) {
		glfwSetWindowAttrib(m_GLFWwindow, GLFW_RESIZABLE, enabled ? GLFW_TRUE : GLFW_FALSE);
	}

	void Window::SetMinSize(Vec2Int size) {
		m_MinSize = { size.x > 0 ? size.x : 0, size.y > 0 ? size.y : 0 };
		glfwSetWindowSizeLimits(m_GLFWwindow,
			m_MinSize.x > 0 ? m_MinSize.x : GLFW_DONT_CARE,
			m_MinSize.y > 0 ? m_MinSize.y : GLFW_DONT_CARE,
			m_MaxSize.x > 0 ? m_MaxSize.x : GLFW_DONT_CARE,
			m_MaxSize.y > 0 ? m_MaxSize.y : GLFW_DONT_CARE);
	}

	void Window::SetMaxSize(Vec2Int size) {
		m_MaxSize = { size.x > 0 ? size.x : 0, size.y > 0 ? size.y : 0 };
		glfwSetWindowSizeLimits(m_GLFWwindow,
			m_MinSize.x > 0 ? m_MinSize.x : GLFW_DONT_CARE,
			m_MinSize.y > 0 ? m_MinSize.y : GLFW_DONT_CARE,
			m_MaxSize.x > 0 ? m_MaxSize.x : GLFW_DONT_CARE,
			m_MaxSize.y > 0 ? m_MaxSize.y : GLFW_DONT_CARE);
	}

	void Window::SetCursorLocked(bool enabled) {
		SetCursorMode(enabled ? 1 : 0);
	}
	void Window::SetCursorHidden(bool enabled) {
		SetCursorMode(enabled ? 3 : 0);
	}
	void Window::SetCursorMode(int mode) {
		int normalized = 0;
		int glfwMode = GLFW_CURSOR_NORMAL;

		switch (mode) {
		case 1:
			normalized = 1;
			glfwMode = GLFW_CURSOR_DISABLED;
			break;
		case 2:
			normalized = 2;
			glfwMode = GLFW_CURSOR_DISABLED;
			break;
		case 3:
			normalized = 3;
			glfwMode = GLFW_CURSOR_HIDDEN;
			break;
		default:
			break;
		}

		glfwSetInputMode(m_GLFWwindow, GLFW_CURSOR, glfwMode);
		m_CursorMode = normalized;
	}
	namespace {
		// 32×32 matches the classic Windows cursor; all project cursors are resampled to this size so source resolution doesn't affect on-screen size.
		constexpr int k_CursorTargetSize = 32;

		unsigned char* ResizeRGBA8Bilinear(const unsigned char* src,
			int srcW, int srcH, int dstW, int dstH)
		{
			if (!src || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return nullptr;
			auto* dst = new unsigned char[static_cast<size_t>(dstW) * dstH * 4];
			const float scaleX = static_cast<float>(srcW) / static_cast<float>(dstW);
			const float scaleY = static_cast<float>(srcH) / static_cast<float>(dstH);
			for (int y = 0; y < dstH; ++y) {
				const float sy = (y + 0.5f) * scaleY - 0.5f;
				const int y0 = std::max(0, static_cast<int>(std::floor(sy)));
				const int y1 = std::min(srcH - 1, y0 + 1);
				const float fy = sy - std::floor(sy);
				for (int x = 0; x < dstW; ++x) {
					const float sx = (x + 0.5f) * scaleX - 0.5f;
					const int x0 = std::max(0, static_cast<int>(std::floor(sx)));
					const int x1 = std::min(srcW - 1, x0 + 1);
					const float fx = sx - std::floor(sx);
					for (int c = 0; c < 4; ++c) {
						const float p00 = src[(y0 * srcW + x0) * 4 + c];
						const float p10 = src[(y0 * srcW + x1) * 4 + c];
						const float p01 = src[(y1 * srcW + x0) * 4 + c];
						const float p11 = src[(y1 * srcW + x1) * 4 + c];
						const float top = p00 + (p10 - p00) * fx;
						const float bot = p01 + (p11 - p01) * fx;
						const float v = top + (bot - top) * fy;
						dst[(y * dstW + x) * 4 + c] = static_cast<unsigned char>(v + 0.5f);
					}
				}
			}
			return dst;
		}

		GLFWcursor* MakeCursorFromTexture(const Texture2D* tex2D) {
			if (!tex2D) return nullptr;
			std::unique_ptr<ImageData> imgData = tex2D->GetImageData();
			if (!imgData || !imgData->Pixels) {
				IDX_WARN_TAG("Window", "Cannot set cursor image from invalid texture data");
				return nullptr;
			}
			if (tex2D->IsFlippedY()) {
				imgData->FlipVerticalRGBA();
			}

			GLFWimage img;
			img.width = k_CursorTargetSize;
			img.height = k_CursorTargetSize;

			std::unique_ptr<unsigned char[]> resized;
			if (imgData->Width == k_CursorTargetSize && imgData->Height == k_CursorTargetSize) {
				img.pixels = imgData->Pixels;
			}
			else {
				resized.reset(ResizeRGBA8Bilinear(imgData->Pixels,
					imgData->Width, imgData->Height,
					k_CursorTargetSize, k_CursorTargetSize));
				if (!resized) {
					IDX_WARN_TAG("Window", "Cursor resize to {}x{} failed",
						k_CursorTargetSize, k_CursorTargetSize);
					return nullptr;
				}
				img.pixels = resized.get();
			}

			return glfwCreateCursor(&img, 0, 0);
		}
	}

	void Window::SetCursorImage(const Texture2D* tex2D) {
		GLFWcursor* fresh = tex2D ? MakeCursorFromTexture(tex2D) : nullptr;
		if (tex2D && !fresh) return;

		m_CursorTextureAssetId = 0;
		const bool wasUsingDefault = (m_Cursor == m_DefaultCursor);
		if (m_DefaultCursor) {
			glfwDestroyCursor(m_DefaultCursor);
		}
		m_DefaultCursor = fresh;

		if (wasUsingDefault) {
			m_Cursor = m_DefaultCursor;
			glfwSetCursor(m_GLFWwindow, m_DefaultCursor);
		}
	}

	void Window::SetUICursorImage(const Texture2D* tex2D) {
		GLFWcursor* fresh = tex2D ? MakeCursorFromTexture(tex2D) : nullptr;
		if (tex2D && !fresh) return;

		const bool wasUsingUI = (m_Cursor == m_UICursor);
		if (m_UICursor) {
			glfwDestroyCursor(m_UICursor);
		}
		m_UICursor = fresh;

		if (wasUsingUI) {
			m_Cursor = m_UICursor;
			glfwSetCursor(m_GLFWwindow, m_UICursor);
		}
	}

	void Window::SetCursorOverUI(bool overUI) {
		if (overUI == m_CursorOverUI) return;
		m_CursorOverUI = overUI;

		GLFWcursor* desired = overUI && m_UICursor ? m_UICursor : m_DefaultCursor;
		if (desired == m_Cursor) return;
		m_Cursor = desired;
		glfwSetCursor(m_GLFWwindow, desired);
	}

	void Window::SetWindowIcon(const Texture2D* tex2D) {
		IDX_ASSERT(tex2D, IndexErrorCode::NullReference, "Texture is null");
		std::unique_ptr<ImageData> imgData(tex2D->GetImageData());
		IDX_ASSERT(imgData, IndexErrorCode::NullReference, "Image data is null");


		const int w = imgData->Width;
		const int h = imgData->Height;

		IDX_ASSERT(w > 0 && h > 0, IndexErrorCode::InvalidValue, "Icon size must be > 0");
		IDX_ASSERT(imgData->Pixels != nullptr, IndexErrorCode::NullReference, "Icon pixels null");

		if (tex2D->IsFlippedY()) {
			imgData->FlipVerticalRGBA();
		}

		GLFWimage img;
		img.width = imgData->Width;
		img.height = imgData->Height;
		img.pixels = imgData->Pixels;

		glfwSetWindowIcon(m_GLFWwindow, 1, &img);
	}

	void Window::SetWindowIconFromResource() {
#ifdef IDX_PLATFORM_WINDOWS
		HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
		if (!hIcon) {
			IDX_WARN_TAG("Window", "No embedded icon resource found.");
			return;
		}

		ICONINFO iconInfo{};
		if (!GetIconInfo(hIcon, &iconInfo)) {
			DestroyIcon(hIcon);
			IDX_WARN_TAG("Window", "Failed to get icon info.");
			return;
		}

		BITMAP bm{};
		GetObject(iconInfo.hbmColor, sizeof(bm), &bm);

		int w = bm.bmWidth;
		int h = bm.bmHeight;

		std::vector<unsigned char> pixels(w * h * 4);

		BITMAPINFOHEADER bi{};
		bi.biSize = sizeof(bi);
		bi.biWidth = w;
		bi.biHeight = -h; // top-down
		bi.biPlanes = 1;
		bi.biBitCount = 32;
		bi.biCompression = BI_RGB;

		HDC hdc = GetDC(NULL);
		GetDIBits(hdc, iconInfo.hbmColor, 0, h, pixels.data(),
			reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
		ReleaseDC(NULL, hdc);

		// Windows gives BGRA, GLFW expects RGBA
		for (int i = 0; i < w * h * 4; i += 4)
			std::swap(pixels[i], pixels[i + 2]);

		DeleteObject(iconInfo.hbmColor);
		DeleteObject(iconInfo.hbmMask);
		DestroyIcon(hIcon);

		GLFWimage img;
		img.width = w;
		img.height = h;
		img.pixels = pixels.data();

		glfwSetWindowIcon(m_GLFWwindow, 1, &img);
#else
		IDX_WARN_TAG("Window", "SetWindowIconFromResource is only supported on Windows.");
#endif
	}

	void Window::SetWindowResizedCallback(GLFWwindow* window, int width, int height) {
		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (!win) return;

		win->SyncViewportFromFramebuffer();
		win->UpdateViewport();

		if (win->m_EventCallback) {
			WindowResizeEvent e(width, height);
			win->m_EventCallback(e);
		}
	}

	Vec2 Window::GetCursorPosition() const {
		double x, y;
		glfwGetCursorPos(m_GLFWwindow, &x, &y);
		return Vec2(x, y);
	}
	std::string Window::GetClipboardString() const {
		const char* clipboard = glfwGetClipboardString(m_GLFWwindow);
		return clipboard ? std::string(clipboard) : std::string{};
	}
	Vec2Int Window::GetScreenCenter() const {
		return Vec2Int(k_Videomode->width / 2, k_Videomode->height / 2);
	}
	Vec2 Window::GetWindowCenter() const {
		Vec2Int size = GetSize();
		return Vec2(size.x / 2.f, size.y / 2.f);
	}
	GLFWmonitor* Window::GetWindowMonitor() const { return glfwGetWindowMonitor(m_GLFWwindow); }
	GLFWmonitor* Window::GetMainMonitor() { return glfwGetPrimaryMonitor(); }

	bool Window::IsMaximized() const {
		return glfwGetWindowAttrib(m_GLFWwindow, GLFW_MAXIMIZED);
	}
	bool Window::IsMinimized() const {
		return glfwGetWindowAttrib(m_GLFWwindow, GLFW_ICONIFIED);
	}
	bool Window::IsFullScreen()const {
		return GetWindowMonitor() != nullptr;
	}
	bool Window::IsVisible() const {
		return glfwGetWindowAttrib(m_GLFWwindow, GLFW_VISIBLE) == GLFW_TRUE;
	}
	bool Window::IsDecorated()const {
		return glfwGetWindowAttrib(m_GLFWwindow, GLFW_DECORATED);
	}
	bool Window::IsResizeable() const {
		return glfwGetWindowAttrib(m_GLFWwindow, GLFW_RESIZABLE);
	}


	void Window::SetDropCallback(GLFWwindow* window, int count, const char** paths) {
		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (!win || !win->m_EventCallback || count <= 0) return;

		std::vector<std::string> filePaths;
		filePaths.reserve(count);
		for (int i = 0; i < count; i++) {
			filePaths.emplace_back(paths[i]);
		}

		FileDropEvent e(std::move(filePaths));
		win->m_EventCallback(e);
	}

	void Window::UpdateViewport() {
		if (!RenderApi::IsInitialized() || !s_MainViewport) {
			return;
		}
		const int fbW = s_MainViewport->GetFramebufferWidth();
		const int fbH = s_MainViewport->GetFramebufferHeight();
		const int x   = s_MainViewport->GetOffsetX();
		const int y   = s_MainViewport->GetOffsetY();
		const int w   = s_MainViewport->GetWidth();
		const int h   = s_MainViewport->GetHeight();
		// OnWindowResize resets the swap chain; SetViewport-only would leave it stale when an FBO is bound mid-frame.
		RenderApi::OnWindowResize(fbW, fbH);
		RenderApi::SetViewport(x, y, w, h);
	}

	void Window::SyncViewportFromFramebuffer() {
		if (!m_GLFWwindow) {
			return;
		}

		int fbW = 0;
		int fbH = 0;
		glfwGetFramebufferSize(m_GLFWwindow, &fbW, &fbH);

		int subW = fbW > 0 ? fbW : 1;
		int subH = fbH > 0 ? fbH : 1;
		int offsetX = 0;
		int offsetY = 0;
		if (m_AspectLock > 0.0f && fbW > 0 && fbH > 0) {
			const float fbAspect = static_cast<float>(fbW) / static_cast<float>(fbH);
			if (fbAspect > m_AspectLock) {
				// Pillarbox: shrink width, keep full height.
				subW = static_cast<int>(std::round(static_cast<float>(fbH) * m_AspectLock));
				subH = fbH;
			} else {
				// Letterbox: keep full width, shrink height.
				subW = fbW;
				subH = static_cast<int>(std::round(static_cast<float>(fbW) / m_AspectLock));
			}
			if (subW < 1) subW = 1;
			if (subH < 1) subH = 1;
			offsetX = (fbW - subW) / 2;
			offsetY = (fbH - subH) / 2;
		}

		if (!s_MainViewport) {
			s_MainViewport = std::make_unique<Viewport>(subW, subH);
		}
		s_MainViewport->SetLetterboxedSubRect(fbW, fbH, offsetX, offsetY, subW, subH);

		// Publish UIRegion only when locked: the editor's Game View panel owns it otherwise; unlocked runtime already uses raw OS-window coords.
		if (m_AspectLock > 0.0f) {
			SetUIRegion(offsetX, offsetY, subW, subH);
		}
	}

	void Window::SetAspectLock(float aspect) {
		m_AspectLock = aspect > 0.0f ? aspect : 0.0f;
		SyncViewportFromFramebuffer();
		UpdateViewport();
	}

	void Window::ResyncViewportAfterRenderApiInit() {
		SyncViewportFromFramebuffer();
		UpdateViewport();
	}

	void Window::SetTitlebarCaptionRect(int x, int y, int w, int h) {
		Win32Titlebar::SetCaptionRect(x, y, w, h);
	}

	void Window::AddTitlebarNonClientRect(int x, int y, int w, int h) {
		Win32Titlebar::AddNonClientRect(x, y, w, h);
	}

	void Window::ResetTitlebarNonClientRects() {
		Win32Titlebar::ResetNonClientRects();
	}

	void Window::SetTitlebarDarkMode(bool enabled) {
		if (m_TitlebarDarkMode == enabled) return;
		m_TitlebarDarkMode = enabled;
		ApplyTitlebarAppearance();
	}

	void Window::ApplyTitlebarAppearance() {
#ifdef IDX_PLATFORM_WINDOWS
		if (!m_GLFWwindow) return;

		const bool focused = glfwGetWindowAttrib(m_GLFWwindow, GLFW_FOCUSED) == GLFW_TRUE;
		Color caption = m_TitlebarColor;
		if (focused && m_TitlebarActiveColor.a > 0.0f) {
			caption = m_TitlebarActiveColor;
		}
		else if (!focused && m_TitlebarInactiveColor.a > 0.0f) {
			caption = m_TitlebarInactiveColor;
		}

		Win32Titlebar::SetDarkMode(m_GLFWwindow, m_TitlebarDarkMode);
		Win32Titlebar::SetColors(m_GLFWwindow, caption, m_TitlebarTextColor, caption);
#endif
	}

	int Window::GetTitlebarHeight() const {
#ifdef IDX_PLATFORM_WINDOWS
		return Win32Titlebar::GetTitlebarHeight();
#else
		return 0;
#endif
	}

	void Window::SetTitlebarHeight(int logicalPx) {
#ifdef IDX_PLATFORM_WINDOWS
		Win32Titlebar::SetTitlebarHeight(logicalPx);
#else
		(void)logicalPx;
#endif
	}
}
