using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Index;

/// <summary>
/// One keyframe of a <see cref="Graph"/>: a point plus cubic-bezier in/out tangent
/// handles (offsets from <see cref="Position"/>). Mirrors the native Index::Curve::Key.
/// </summary>
public struct CurveKey
{
    public Vector2 Position;
    public Vector2 InTangent;   // handle offset from Position, points left (x <= 0)
    public Vector2 OutTangent;  // handle offset from Position, points right (x >= 0)

    public CurveKey(Vector2 position, Vector2 inTangent, Vector2 outTangent)
    {
        Position = position;
        InTangent = inTangent;
        OutTangent = outTangent;
    }
}

/// <summary>
/// An editable animation curve (x in [0,1] -> value) shown as a graph in the editor
/// inspector. Declare a public field of this type on an <see cref="EntityScript"/> to
/// expose it; mirrors the native Index::Curve and serializes inline with the script
/// field. Sample it at runtime with <see cref="Evaluate"/>.
/// </summary>
public sealed class Graph
{
    public List<CurveKey> Keys = DefaultKeys();

    public static List<CurveKey> DefaultKeys() => new()
    {
        new CurveKey(new Vector2(0f, 1f), new Vector2(-0.2f, 0f), new Vector2(0.2f, 0f)),
        new CurveKey(new Vector2(1f, 1f), new Vector2(-0.2f, 0f), new Vector2(0.2f, 0f)),
    };

    /// <summary>Curve value at <paramref name="t01"/> (the x parameter), clamped to the key range.</summary>
    public float Evaluate(float t01)
    {
        if (Keys == null || Keys.Count == 0) return 1f;
        if (Keys.Count == 1 || t01 <= Keys[0].Position.X) return Keys[0].Position.Y;
        if (t01 >= Keys[Keys.Count - 1].Position.X) return Keys[Keys.Count - 1].Position.Y;

        for (int i = 1; i < Keys.Count; i++)
        {
            if (t01 <= Keys[i].Position.X)
            {
                CurveKey a = Keys[i - 1], b = Keys[i];
                Vector2 p0 = a.Position;
                Vector2 p1 = new(a.Position.X + a.OutTangent.X, a.Position.Y + a.OutTangent.Y);
                Vector2 p2 = new(b.Position.X + b.InTangent.X, b.Position.Y + b.InTangent.Y);
                Vector2 p3 = b.Position;
                // x is monotonic in t (the editor clamps handle x): bisect for t(x).
                float lo = 0f, hi = 1f;
                for (int iter = 0; iter < 16; iter++)
                {
                    float mid = 0.5f * (lo + hi);
                    if (BezierX(p0, p1, p2, p3, mid) < t01) lo = mid; else hi = mid;
                }
                return BezierY(p0, p1, p2, p3, 0.5f * (lo + hi));
            }
        }
        return Keys[Keys.Count - 1].Position.Y;
    }

    private static float BezierX(Vector2 p0, Vector2 p1, Vector2 p2, Vector2 p3, float t)
    {
        float u = 1f - t;
        return u * u * u * p0.X + 3f * u * u * t * p1.X + 3f * u * t * t * p2.X + t * t * t * p3.X;
    }

    private static float BezierY(Vector2 p0, Vector2 p1, Vector2 p2, Vector2 p3, float t)
    {
        float u = 1f - t;
        return u * u * u * p0.Y + 3f * u * u * t * p1.Y + 3f * u * t * t * p2.Y + t * t * t * p3.Y;
    }

    // Wire format shared verbatim with the native editor/serializer: keys ';'-joined,
    // each key's six floats ','-joined as posX,posY,inX,inY,outX,outY (InvariantCulture).
    internal string Serialize()
    {
        var ic = CultureInfo.InvariantCulture;
        var sb = new StringBuilder();
        for (int i = 0; i < Keys.Count; i++)
        {
            if (i > 0) sb.Append(';');
            CurveKey k = Keys[i];
            sb.Append(k.Position.X.ToString(ic)).Append(',')
              .Append(k.Position.Y.ToString(ic)).Append(',')
              .Append(k.InTangent.X.ToString(ic)).Append(',')
              .Append(k.InTangent.Y.ToString(ic)).Append(',')
              .Append(k.OutTangent.X.ToString(ic)).Append(',')
              .Append(k.OutTangent.Y.ToString(ic));
        }
        return sb.ToString();
    }

    // Total: empty/garbage yields a default curve (never null), since the field-edit
    // path skips null and would otherwise silently drop the field.
    internal static Graph Deserialize(string s)
    {
        var g = new Graph();
        if (string.IsNullOrWhiteSpace(s)) return g;

        var ic = CultureInfo.InvariantCulture;
        var keys = new List<CurveKey>();
        foreach (string part in s.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            string[] f = part.Split(',');
            if (f.Length < 6) continue;
            if (float.TryParse(f[0], NumberStyles.Float, ic, out float px)
                && float.TryParse(f[1], NumberStyles.Float, ic, out float py)
                && float.TryParse(f[2], NumberStyles.Float, ic, out float ix)
                && float.TryParse(f[3], NumberStyles.Float, ic, out float iy)
                && float.TryParse(f[4], NumberStyles.Float, ic, out float ox)
                && float.TryParse(f[5], NumberStyles.Float, ic, out float oy))
            {
                keys.Add(new CurveKey(new Vector2(px, py), new Vector2(ix, iy), new Vector2(ox, oy)));
            }
        }
        if (keys.Count >= 2) g.Keys = keys;
        return g;
    }
}
