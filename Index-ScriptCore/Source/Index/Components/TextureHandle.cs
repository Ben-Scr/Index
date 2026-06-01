using System.Runtime.InteropServices;

namespace Index.Components;

// Layout MUST match Index-Engine/src/Graphics/TextureHandle.hpp; Index == ushort.MaxValue is the invalid sentinel matching C++ TextureHandle::Invalid().
[StructLayout(LayoutKind.Sequential)]
public struct TextureHandle
{
    public ushort Index;
    public ushort Generation;

    public const ushort InvalidIndex = ushort.MaxValue;
    public bool IsValid => Index != InvalidIndex;
}
