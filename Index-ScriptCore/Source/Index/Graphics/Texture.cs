using System;
using Index.Interop;

namespace Index;

// Sampler filter mode mirrored from `Index-Engine/src/Graphics/Filter.hpp`.
// Underlying type MUST stay int — the native `enum class Filter` has no
// explicit underlying type, so MSVC picks int (4 bytes), and the blittable
// `NativeSpriteRenderer.FilterMode` / `NativeImage.FilterMode` layouts
// depend on the 4-byte size matching.
public enum TextureFilter
{
    Point = 0,
    Bilinear = 1,
    Trilinear = 2,
    Anisotropic = 3,
}

// Order MUST match Index-Engine/src/Graphics/DefaultTexture.hpp; adding an entry requires 4 sync sites.
public enum DefaultTexture : byte
{
    Square = 0,
    Pixel = 1,
    Circle = 2,
    Capsule = 3,
    IsometricDiamond = 4,
    HexagonFlatTop = 5,
    HexagonPointedTop = 6,
    NineSliced = 7,
    Invisible = 8,
}

public sealed class Texture : IEquatable<Texture>
{
    public ulong UUID { get; }

    internal Texture(ulong assetId)
    {
        UUID = assetId;
    }

    public bool IsValid => UUID != 0 && InternalCalls.Texture_LoadAsset(UUID);

    public string Name
    {
        get
        {
            if (UUID == 0)
                return AssetDisplay.NoneLabel;

            string name = InternalCalls.Asset_GetDisplayName(UUID);
            return string.IsNullOrEmpty(name) ? "(Missing Asset)" : name;
        }
    }

    public string Path => UUID != 0 ? InternalCalls.Asset_GetPath(UUID) : "";
    public int Width => UUID != 0 ? InternalCalls.Texture_GetWidth(UUID) : 0;
    public int Height => UUID != 0 ? InternalCalls.Texture_GetHeight(UUID) : 0;

    // Public so packages built in separate assemblies (Pkg.<Name>.dll)
    // can construct managed Texture wrappers from a stored asset GUID
    // without re-implementing the AssetRegistry lookup.
    public static Texture? FromAssetUUID(ulong assetId)
    {
        if (assetId == 0 || !InternalCalls.Asset_IsValid(assetId))
            return null;

        return InternalCalls.Texture_LoadAsset(assetId) ? new Texture(assetId) : null;
    }

    public static Texture? GetDefault(DefaultTexture defaultTexture)
    {
        ulong assetId = InternalCalls.Texture_GetDefaultAssetUUID((byte)defaultTexture);
        return FromAssetUUID(assetId);
    }

    public static Texture? Square            => GetDefault(DefaultTexture.Square);
    public static Texture? Pixel             => GetDefault(DefaultTexture.Pixel);
    public static Texture? Circle            => GetDefault(DefaultTexture.Circle);
    public static Texture? Capsule           => GetDefault(DefaultTexture.Capsule);
    public static Texture? IsometricDiamond  => GetDefault(DefaultTexture.IsometricDiamond);
    public static Texture? HexagonFlatTop    => GetDefault(DefaultTexture.HexagonFlatTop);
    public static Texture? HexagonPointedTop => GetDefault(DefaultTexture.HexagonPointedTop);
    public static Texture? NineSliced        => GetDefault(DefaultTexture.NineSliced);
    public static Texture? Invisible         => GetDefault(DefaultTexture.Invisible);

    public bool Equals(Texture? other) => other is not null && UUID == other.UUID;
    public override bool Equals(object? obj) => obj is Texture other && Equals(other);
    public override int GetHashCode() => UUID.GetHashCode();
    public override string ToString() => Name;
}
