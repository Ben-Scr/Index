using System;

namespace Index;

/// <summary>
/// Provides static access to the engine's OS window: dimensions, title,
/// position, fullscreen / maximised state, focus, and a resize event.
/// Maps to the native <c>Index::Window</c>.
/// </summary>
public static class Window
{
    /// <summary>
    /// Fired same-frame as the GLFW framebuffer-size callback when the
    /// user resizes the window. Skipped while minimised (zero-area
    /// frame) so subscribers don't see a transient 0×0.
    /// </summary>
    public static event Action? OnResize;
    public static event Action<bool>? FocusChanged;
    public static event Action? OnMaximize;
    public static event Action? OnMinimize;
    public static event Action? OnRestore;

    internal static void RaiseRestore() => OnRestore?.Invoke();
    internal static void RaiseMinimize() => OnMinimize?.Invoke();
    internal static void RaiseMaximize() => OnMaximize?.Invoke();

    internal static void RaiseFocusChanged(bool focused) => FocusChanged?.Invoke(focused);


    public static int Width => InternalCalls.Window_GetWidth();
    public static int Height => InternalCalls.Window_GetHeight();

    // Cached so the steady-state getter doesn't pay two native calls per
    // read. Seeded on first access (one-time cost), then refreshed in
    // InvokeResize before subscribers run so OnResize handlers see the
    // new size.
    private static Vector2 s_Size;
    private static bool s_SizeCached;

    public static Vector2 Size
    {
        get
        {
            if (!s_SizeCached)
            {
                s_Size = new Vector2(InternalCalls.Window_GetWidth(), InternalCalls.Window_GetHeight());
                s_SizeCached = true;
            }
            return s_Size;
        }
    }

    public static string Title
    {
        get => InternalCalls.Window_GetTitle();
        set => InternalCalls.Window_SetTitle(value);
    }

    public static void Minimize() => InternalCalls.Window_Minimize();
    public static void Maximize() => InternalCalls.Window_Maximize();
    public static void Restore() => InternalCalls.Window_Restore();
    public static void Focus() => InternalCalls.Window_Focus();

    public static bool IsMaximized => InternalCalls.Window_IsMaximized();

    public static bool IsFullScreen
    {
        get => InternalCalls.Window_IsFullScreen();
        set => InternalCalls.Window_SetFullScreen(value);
    }

    public static Vector2Int Position
    {
        get => InternalCalls.Window_GetPosition();
        set => InternalCalls.Window_SetPosition(value);
    }


    /// <summary>
    /// Half-extent of the window in pixels — useful for centring HUD
    /// elements relative to the window itself.
    /// </summary>
    public static Vector2Int WindowCenter => new Vector2Int(Width / 2, Height / 2);

    /// <summary>
    /// Half-extent of the primary monitor's video mode — useful for
    /// centring the window on the user's desktop regardless of its own
    /// size. Pulled from GLFW's video mode the same way the engine's
    /// internal <c>Window::GetScreenCenter()</c> does, so values stay
    /// in sync if the monitor changes.
    /// </summary>
    public static Vector2Int ScreenCenter
    {
        get
        {
            Vector2Int size = InternalCalls.Window_GetScreenSize();
            return new Vector2Int(size.X / 2, size.Y / 2);
        }
    }

    internal static void InvokeResize()
    {
        s_Size = new Vector2(InternalCalls.Window_GetWidth(), InternalCalls.Window_GetHeight());
        s_SizeCached = true;
        OnResize?.Invoke();
    }
}
