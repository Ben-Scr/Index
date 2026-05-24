
namespace Index;

public enum CursorMode
{
    /// <summary>Cursor is visible and behaves normally.</summary>
    Default = 0,
    /// <summary>Cursor is locked to the center of the window.</summary>
    Locked = 1
}

public static class Cursor
{
    public static CursorMode Mode
    {
        get => (CursorMode)InternalCalls.Cursor_GetMode();
        set => InternalCalls.Cursor_SetMode((int)value);
    }

    public static bool Visible
    {
        get => InternalCalls.Cursor_GetMode() == (int)CursorMode.Default;
        set => InternalCalls.Cursor_SetMode(value ? (int)CursorMode.Default : k_CursorModeHidden);
    }

    /// <summary>The current texture of the cursor</summary>
    public static Texture? Texture
    {
        get => Texture.FromAssetUUID(InternalCalls.Cursor_GetTexture());
        set => InternalCalls.Cursor_SetTexture(value?.UUID ?? 0);
    }

    // Native mode 3 = GLFW_CURSOR_HIDDEN (invisible, free movement). Kept off
    // the CursorMode enum because hide-without-lock is a visibility concern,
    // not a meaningful input mode like Default/Locked.
    private const int k_CursorModeHidden = 3;
}

