using Index.Interop;

namespace Index.Graphics;

public static class Gizmo
{
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
    /// Maximum number of gizmo "vertices" that may be registered per frame,
    /// summed across ALL gizmo draws (lines, squares, circles). Once the cap
    /// is reached, any further DrawLine/DrawSquare/DrawCircle calls are
    /// silently dropped for the rest of that frame — so a dense gizmo scene
    /// can appear to "stop drawing" partway through. Raise this if that
    /// happens. Default: 100000.
    /// Cost per draw: a line = 1, a square = 4, a circle = its segment count
    /// (DrawCircle's <c>segments</c>, default 32).
    /// </summary>
    public static int MaxVertices
    {
        get => InternalCalls.Gizmo_GetMaxVertices();
        set => InternalCalls.Gizmo_SetMaxVertices(value);
    }

    /// <summary>
    /// Gizmo vertices registered so far in the current frame (read-only).
    /// Compare against <see cref="MaxVertices"/> to gauge how close you are
    /// to the per-frame cap.
    /// </summary>
    public static int RegisteredVertices => InternalCalls.Gizmo_GetRegisteredVertices();


    public static void DrawLine(Vector2 start, Vector2 end)
        => InternalCalls.Gizmo_DrawLine(start.X, start.Y, end.X, end.Y);

    public static void DrawSquare(Vector2 center, Vector2 size, float degrees = 0.0f)
        => InternalCalls.Gizmo_DrawSquare(center.X, center.Y, size.X, size.Y, degrees);

    public static void DrawCircle(Vector2 center, float radius, int segments = 32)
        => InternalCalls.Gizmo_DrawCircle(center.X, center.Y, radius, segments);
}
