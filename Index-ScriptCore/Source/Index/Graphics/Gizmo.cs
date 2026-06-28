using Index.Interop;

namespace Index.Graphics;

public static class Gizmo
{
    /// <summary>
    /// Master switch for gizmo drawing. When false, every Gizmo.Draw* call is a
    /// no-op for the rest of the session until set back to true.
    /// </summary>
    public static bool IsEnabled
    {
        get => InternalCalls.Gizmo_GetEnabled();
        set => InternalCalls.Gizmo_SetEnabled(value);
    }

    public static Color Color
    {
        get => GetColor();
        set => InternalCalls.Gizmo_SetColor(value.R, value.G, value.B, value.A);
    }

    internal static Color GetColor()
    {
        InternalCalls.Gizmo_GetColor(out float r, out float g, out float b, out float a);
        return new Color(r, g, b, a);
    }

    public static float LineWidth
    {
        get => InternalCalls.Gizmo_GetLineWidth();
        set => InternalCalls.Gizmo_SetLineWidth(value);
    }

    /// <summary>
    /// Per-frame render budget, summed across gizmo draws (lines, squares, circles,
    /// text). Enforced per view on the VISIBLE gizmos only — off-screen gizmos are
    /// frustum-culled BEFORE counting, so drawing thousands of off-screen gizmos can't
    /// starve the on-screen ones. If a single view's on-screen gizmos exceed the
    /// budget, the overflow is dropped (with a one-time log warning). Default: 100,000.
    /// Cost: a line = 1, a (wire or filled) square = 4, a (wire or filled) circle = its
    /// segment count (default 32), text = its length.
    /// </summary>
    public static int MaxVertices
    {
        get => InternalCalls.Gizmo_GetMaxVertices();
        set => InternalCalls.Gizmo_SetMaxVertices(value);
    }

    /// <summary>
    /// Total gizmo draws accumulated so far this frame (read-only, pre-cull). This is
    /// the raw count across all draws; the render budget (<see cref="MaxVertices"/>) is
    /// enforced separately, per view, on the on-screen subset.
    /// </summary>
    public static int RegisteredVertices => InternalCalls.Gizmo_GetRegisteredVertices();


    public static void DrawLine(Vector2 start, Vector2 end)
        => InternalCalls.Gizmo_DrawLine(start.X, start.Y, end.X, end.Y);

    public static void DrawLine(Vector2 start, Vector2 direction, float distance)
        => throw new NotImplementedException();

    /// <summary>Outline-only square. <paramref name="degrees"/> rotates about the center.</summary>
    public static void DrawWireSquare(Vector2 center, Vector2 size, float degrees = 0.0f)
        => InternalCalls.Gizmo_DrawWireSquare(center.X, center.Y, size.X, size.Y, degrees);

    /// <summary>Outline-only circle approximated with <paramref name="segments"/> edges.</summary>
    public static void DrawWireCircle(Vector2 center, float radius, int segments = 32)
        => InternalCalls.Gizmo_DrawWireCircle(center.X, center.Y, radius, segments);

    /// <summary>Solid (filled) square. <paramref name="degrees"/> rotates about the center.</summary>
    public static void DrawSquare(Vector2 center, Vector2 size, float degrees = 0.0f)
        => InternalCalls.Gizmo_DrawSquare(center.X, center.Y, size.X, size.Y, degrees);

    /// <summary>Solid (filled) circle approximated with <paramref name="segments"/> triangles.</summary>
    public static void DrawCircle(Vector2 center, float radius, int segments = 32)
        => InternalCalls.Gizmo_DrawCircle(center.X, center.Y, radius, segments);

    /// <summary>
    /// World-space text centered on <paramref name="position"/>, using the project's
    /// default font and the current <see cref="Color"/>. <paramref name="rotation"/> is
    /// in degrees (about the position); <paramref name="size"/> is in pixels (same
    /// convention as a Text component's font size).
    /// </summary>
    public static void DrawText(Vector2 position, string text, float rotation = 0.0f, float size = 28.0f)
        => InternalCalls.Gizmo_DrawText(text, position.X, position.Y, rotation, size);
}
