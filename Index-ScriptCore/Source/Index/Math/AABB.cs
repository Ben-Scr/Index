using System;
using System.Runtime.InteropServices;

namespace Index;

// World-space axis-aligned box. Mirrors the engine-side Index::AABB (Min/Max corners).
[StructLayout(LayoutKind.Explicit, Size = 16)]
public struct AABB : IEquatable<AABB>
{
    [FieldOffset(0)] public Vector2 Min;
    [FieldOffset(8)] public Vector2 Max;

    public AABB(Vector2 min, Vector2 max) { Min = min; Max = max; }

    public static AABB FromCenterSize(Vector2 center, Vector2 size)
    {
        Vector2 half = size * 0.5f;
        return new AABB(center - half, center + half);
    }

    public Vector2 Center => (Min + Max) * 0.5f;
    public Vector2 Size => Max - Min;
    public Vector2 Extents => (Max - Min) * 0.5f;
    public float Width => Max.X - Min.X;
    public float Height => Max.Y - Min.Y;

    public bool Contains(Vector2 point) =>
        point.X >= Min.X && point.X <= Max.X &&
        point.Y >= Min.Y && point.Y <= Max.Y;

    public bool Intersects(AABB other) =>
        Min.X <= other.Max.X && Max.X >= other.Min.X &&
        Min.Y <= other.Max.Y && Max.Y >= other.Min.Y;

    public bool Equals(AABB other) => Min.Equals(other.Min) && Max.Equals(other.Max);
    public override bool Equals(object? obj) => obj is AABB other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(Min.X, Min.Y, Max.X, Max.Y);
    public override string ToString() => $"AABB(Min: {Min}, Max: {Max})";

    public static bool operator ==(AABB a, AABB b) => a.Equals(b);
    public static bool operator !=(AABB a, AABB b) => !a.Equals(b);
}
