#pragma once
#include "Collections/Color.hpp"
#include "Collections/Vec2.hpp"
#include "Collections/Viewport.hpp"
#include "Core/Export.hpp"
#include "Core/WindowSpecification.hpp"
#include "Events/IndexEvent.hpp"

#include <GLFW/glfw3.h>
#include <cstdint>
#include <string>
#include <memory>
#include <functional>

namespace Index {
	class Texture2D;

	class INDEX_API Window {
		friend class Application;

	public:
		Window(int width, int height, const std::string& title);
		Window(const WindowSpecification& props);

		// Info: Initializes the GLFW library
		static void Initialize();
		// Info: Terminates the GLFW library
		static void Shutdown();
		static bool IsInitialized() { return s_IsInitialized; }

		static void SetVsync(bool enabled);

		void Destroy();

		void SetClipboardString(const std::string& s) { glfwSetClipboardString(m_GLFWwindow, s.c_str()); }
		void SetWindowResizeable(bool enabled) { glfwSetWindowAttrib(m_GLFWwindow, GLFW_RESIZABLE, enabled ? GLFW_TRUE : GLFW_FALSE); }
		void SetWindowMoveable(bool enabled) { glfwSetWindowAttrib(m_GLFWwindow, GLFW_DECORATED, enabled ? GLFW_TRUE : GLFW_FALSE); }
		void SetPosition(Vec2Int pos) { glfwSetWindowPos(m_GLFWwindow, pos.x, pos.y); }
		void SetSize(Vec2Int size) { glfwSetWindowSize(m_GLFWwindow, size.x, size.y); }

		// Components <= 0 are treated as GLFW_DONT_CARE; cached because GLFW has no getter for size limits.
		void SetMinSize(Vec2Int size);
		void SetMaxSize(Vec2Int size);
		Vec2Int GetMinSize() const { return m_MinSize; }
		Vec2Int GetMaxSize() const { return m_MaxSize; }
		void SetCursorPosition(Vec2 position) { glfwSetCursorPos(m_GLFWwindow, (double)position.x, (double)position.y); }

		void SetCursorLocked(bool enabled);
		void SetCursorHidden(bool enabled);
		void SetCursorMode(int mode);
		int GetCursorMode() const { return m_CursorMode; }

		void SetUICursorImage(const Texture2D* tex2D);
		void SetCursorOverUI(bool overUI);


		void SetCursorImage(const Texture2D* tex2D);
		// Editor-only gate. When false, SetCursorImage / SetUICursorImage /
		// SetCursorOverUI keep their state but DON'T touch the OS cursor, so the
		// project's game cursor stays scoped to the Game View instead of leaking
		// editor-wide. Always true in standalone builds; the editor flips it true
		// only while the Game View is hovered/focused.
		void SetGameCursorEnabled(bool enabled);
		bool HasGameCursor() const { return m_DefaultCursor != nullptr; }
		uint64_t GetCursorTextureAsset() const { return m_CursorTextureAssetId; }
		void SetCursorTextureAsset(uint64_t assetId) { m_CursorTextureAssetId = assetId; }
		void SetWindowIcon(const Texture2D* tex2D);
		void SetWindowIconFromResource();

		// Info: If enabled equals true the window will become fullscreen else windowed
		void SetFullScreen(bool enabled);
		void SetDecorated(bool enabled);
		void SetVisible(bool enabled);
		void SetResizeable(bool enabled);
		void SetTitle(const std::string& title) { glfwSetWindowTitle(m_GLFWwindow, title.c_str()); }

		void SetAspectLock(float aspect);
		float GetAspectLock() const { return m_AspectLock; }

		// MUST be called once after RenderApi::Init: the framebuffer-size callback doesn't fire on initial window creation, so frame 0 would miss the aspect-lock sub-rect without this.
		void ResyncViewportAfterRenderApiInit();

		std::string GetTitle() const { return std::string(glfwGetWindowTitle(m_GLFWwindow)); }
		int GetWidth()  const { return s_MainViewport->GetWidth(); }
		int GetHeight() const { return s_MainViewport->GetHeight(); }
		GLFWmonitor* GetWindowMonitor() const;
		static GLFWmonitor* GetMainMonitor();
		const GLFWvidmode* GetVideomode() const { return k_Videomode; }
		GLFWwindow* GetGLFWWindow() const { return m_GLFWwindow; }
		float GetAspect() const { return s_MainViewport->GetAspect(); }
		Vec2Int GetSize() const { return s_MainViewport->GetSize(); }
		Vec2Int GetPosition() const {
			Vec2Int pos;
			glfwGetWindowPos(m_GLFWwindow, &pos.x, &pos.y);
			return pos;
		}

		std::string GetClipboardString() const;
		Vec2 GetCursorPosition() const;


		bool IsMaximized() const;
		bool IsMinimized() const;
		bool IsFullScreen() const;
		bool IsVisible() const;
		bool IsDecorated() const;
		bool IsResizeable() const;

		bool IsCustomTitlebarEnabled() const { return m_CustomTitlebar; }
		int GetTitlebarHeight() const;
		void SetTitlebarHeight(int logicalPx);

		// Alpha = 0 means "no override"; applied via DWM on native chrome or read by the custom ImGui titlebar.
		Color GetTitlebarColor() const { return m_TitlebarColor; }
		void  SetTitlebarColor(Color color) { m_TitlebarColor = color; ApplyTitlebarAppearance(); }
		Color GetTitlebarTextColor() const { return m_TitlebarTextColor; }
		void  SetTitlebarTextColor(Color color) { m_TitlebarTextColor = color; ApplyTitlebarAppearance(); }
		Color GetTitlebarActiveColor() const { return m_TitlebarActiveColor; }
		void  SetTitlebarActiveColor(Color color) { m_TitlebarActiveColor = color; ApplyTitlebarAppearance(); }
		Color GetTitlebarInactiveColor() const { return m_TitlebarInactiveColor; }
		void  SetTitlebarInactiveColor(Color color) { m_TitlebarInactiveColor = color; ApplyTitlebarAppearance(); }
		bool IsTitlebarDarkMode() const { return m_TitlebarDarkMode; }
		void SetTitlebarDarkMode(bool enabled);

		static bool IsVsync() { return s_IsVsync; }

		// Info: If reset equals true the window will automatically be restored if it's already maximized
		void MaximizeWindow(bool reset = false);
		void MinimizeWindow();
		void RestoreWindow();

		void CenterWindow();
		void Focus();

		void SetEventCallback(const EventCallbackFn& callback) { m_EventCallback = callback; }

		static Window* GetActiveWindow() { return s_ActiveWindow; }
		static Viewport* GetMainViewport() { return s_MainViewport.get(); }

		// Panel pixel rect (OS-window space) published by the editor each frame so UI hit-tests use game-panel coords instead of OS-window coords. Inactive in standalone builds.
		struct UIRegion {
			int OffsetX = 0;
			int OffsetY = 0;
			int Width = 0;
			int Height = 0;
			bool IsActive() const { return Width > 0 && Height > 0; }
		};

		static UIRegion GetUIRegion() { return s_UIRegion; }
		static void SetUIRegion(int x, int y, int w, int h) {
			s_UIRegion = UIRegion{ x, y, w, h };
		}
		static void ClearUIRegion() { s_UIRegion = UIRegion{}; }

		static void SetTitlebarCaptionRect(int x, int y, int w, int h);
		static void AddTitlebarNonClientRect(int x, int y, int w, int h);
		static void ResetTitlebarNonClientRects();

	private:
		void Create(const WindowSpecification& props);
		void ApplyTitlebarAppearance();
		void SyncViewportFromFramebuffer();
		void UpdateViewport();
		// GLFW_NO_API window — glfwSwapBuffers is not available; delegates to RenderApi::Present().
		void SwapBuffers() const;

		bool ShouldClose() const { return glfwWindowShouldClose(m_GLFWwindow); }
		Vec2Int GetScreenCenter() const;
		Vec2 GetWindowCenter() const;

		static void FocusCallback(GLFWwindow* window, int focused);
		static void RefreshCallback(GLFWwindow* window);
		static void IconifyCallback(GLFWwindow* window, int iconified);
		static void CloseCallback(GLFWwindow* window);
		static void SetKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
		static void SetCharCallback(GLFWwindow* window, unsigned int codepoint);
		static void SetMouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
		static void SetCursorPositionCallback(GLFWwindow* window, double xPos, double yPos);
		static void SetScrollCallback(GLFWwindow* window, double xOffset, double yOffset);
		static void SetWindowResizedCallback(GLFWwindow* window, int width, int height);
		static void SetDropCallback(GLFWwindow* window, int count, const char** paths);

		static Window* s_ActiveWindow;

		GLFWwindow* m_GLFWwindow = nullptr;
		GLFWcursor* m_Cursor = nullptr;
		GLFWcursor* m_DefaultCursor = nullptr;
		GLFWcursor* m_UICursor = nullptr;
		bool m_CursorOverUI = false;
		// Runtime: always true. Editor: false except while over the Game View.
		bool m_GameCursorEnabled = true;
		int m_CursorMode = 0;
		uint64_t m_CursorTextureAssetId = 0;

		Vec2Int m_RestoreSize;
		Vec2Int m_RestorePos;
		Vec2Int m_MinSize;
		Vec2Int m_MaxSize;
		bool m_CustomTitlebar = false;
		float m_AspectLock = 0.0f;
		Color m_TitlebarColor         { 0.0f, 0.0f, 0.0f, 0.0f };
		Color m_TitlebarTextColor     { 0.0f, 0.0f, 0.0f, 0.0f };
		Color m_TitlebarActiveColor   { 0.0f, 0.0f, 0.0f, 0.0f };
		Color m_TitlebarInactiveColor { 0.0f, 0.0f, 0.0f, 0.0f };
		bool m_TitlebarDarkMode = true;

		static const GLFWvidmode* k_Videomode;

		static std::unique_ptr<Viewport> s_MainViewport;
		static UIRegion s_UIRegion;
		static bool s_IsVsync;
		static bool s_IsInitialized;

		EventCallbackFn m_EventCallback;
	};
}
