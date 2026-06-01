using System.Runtime.InteropServices;
using Index.Components;

namespace Index.Native;

// Layout MUST match Transform2DComponent.hpp; ScriptHostBridge verifies sizeof at init. Setters mirror root-entity writes into Local* and mark dirty so ref-API edits survive the hierarchy pass.
[StructLayout(LayoutKind.Sequential)]
public struct NativeTransform2D : IComponent
{
    // WARNING: `default(NativeTransform2D)` bypasses this constructor — Scale=(0,0). Use ecb.CreateEntityWith<> instead of ecb.AddComponent<>(e, default).
    public NativeTransform2D() { }

    private Vector2 m_Position;
    private Vector2 m_Scale = new(1f, 1f);
    private float  m_RotationRadians;

    private Vector2 m_LocalPosition;
    private Vector2 m_LocalScale = new(1f, 1f);
    private float  m_LocalRotationRadians;

    // m_Dirty defaults true to match C++ Transform2DComponent.hpp; m_OwnerScene/m_OwnerEntity exist only to keep managed layout byte-identical to C++.
    [MarshalAs(UnmanagedType.U1)]
    private bool m_Dirty = true;
    private nint m_OwnerScene;
    private uint m_OwnerEntity;

    public Vector2 Position
    {
        readonly get => m_Position;
        set
        {
            m_Position = value;
            m_LocalPosition = value;
            m_Dirty = true;
        }
    }

    public Vector2 Scale
    {
        readonly get => m_Scale;
        set
        {
            m_Scale = value;
            m_LocalScale = value;
            m_Dirty = true;
        }
    }

    public Vector2 LocalPosition
    {
        readonly get => m_LocalPosition;
        set
        {
            m_LocalPosition = value;
            m_Position = value;
            m_Dirty = true;
        }
    }

    public Vector2 LocalScale
    {
        readonly get => m_LocalScale;
        set
        {
            m_LocalScale = value;
            m_Scale = value;
            m_Dirty = true;
        }
    }

    public bool Dirty
    {
        readonly get => m_Dirty;
        set => m_Dirty = value;
    }

    public float Rotation
    {
        readonly get => m_RotationRadians * Mathf.Rad2Deg;
        set
        {
            m_RotationRadians = value * Mathf.Deg2Rad;
            m_LocalRotationRadians = m_RotationRadians;
            m_Dirty = true;
        }
    }

    public float LocalRotation
    {
        readonly get => m_LocalRotationRadians * Mathf.Rad2Deg;
        set
        {
            m_LocalRotationRadians = value * Mathf.Deg2Rad;
            m_RotationRadians = m_LocalRotationRadians;
            m_Dirty = true;
        }
    }

    // Direction vectors derived from world Rotation (0 degrees -> Up = (0, 1)).
    public readonly Vector2 Up    => new(-Mathf.Sin(m_RotationRadians), Mathf.Cos(m_RotationRadians));
    public readonly Vector2 Down  => -Up;
    public readonly Vector2 Right => new(Mathf.Cos(m_RotationRadians),  Mathf.Sin(m_RotationRadians));
    public readonly Vector2 Left  => -Right;

    public readonly float RotationDegrees => Rotation;

    // Native serialized/display name used by the binding layer to find this
    // component's pool.
    internal const string NativeName = "Transform 2D";

    public static NativeTransform2D FromPosition(Vector2 position) => new() { Position = position };
    public static NativeTransform2D FromScale(Vector2 scale) => new() { Scale = scale };
    public static NativeTransform2D FromRotation(float rotationDegrees) => new() { Rotation = rotationDegrees };
    public static NativeTransform2D FromPositionScaleRotation(Vector2 position, Vector2 scale, float rotationDegrees) => new() {
        Position = position,
        Scale = scale,
        Rotation = rotationDegrees
    };

    public void SetPosition(Vector2 position)
    {
        Position = position;
    }
    public void SetScale(float scale)
    {
        Scale = new Vector2(scale, scale);
    }
    public void SetRotation(float rotationDegrees)
    {
        Rotation = rotationDegrees;
    }
}
