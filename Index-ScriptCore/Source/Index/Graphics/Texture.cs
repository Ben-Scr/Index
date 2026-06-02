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

public sealed class Texture : IEquatable<Texture>, IDisposable
{
    public ulong UUID { get; private set; }

    // Runtime/procedural state. Non-null only for textures created via
    // Texture.Create — the managed byte[] IS the source of truth (RGBA8,
    // 4 bytes per pixel, row-major, top-left origin). SetPixel/GetPixel
    // mutate it without crossing the FFI; Apply() uploads it to the GPU in
    // one call. Asset (file-backed) textures keep all of these null/zero.
    private byte[]? _pixels;
    private readonly int _runtimeWidth;
    private readonly int _runtimeHeight;

    /// <summary>True for a script-created (procedural) texture — owns a CPU
    /// pixel buffer and must be Dispose()d to free its GPU resource.</summary>
    public bool IsRuntime => _pixels != null;

    internal Texture(ulong assetId)
    {
        UUID = assetId;
    }

    private Texture(ulong runtimeUuid, int width, int height, byte[] pixels)
    {
        UUID = runtimeUuid;
        _runtimeWidth = width;
        _runtimeHeight = height;
        _pixels = pixels;
    }

    /// <summary>
    /// Create a blank (transparent-black) procedural texture you can paint
    /// into with <see cref="SetPixel"/> / <see cref="SetPixels"/>, then push
    /// to the GPU with <see cref="Apply"/>. Assign it to a SpriteRenderer /
    /// Image like any other texture. Call <see cref="Dispose"/> (or use a
    /// `using`) to free the GPU resource — it is NOT garbage-collected.
    /// </summary>
    public static Texture? Create(int width, int height, TextureFilter filter = TextureFilter.Point)
    {
        if (width <= 0 || height <= 0)
        {
            Log.Error($"Texture.Create: invalid size {width}x{height}.");
            return null;
        }

        byte[] pixels = new byte[width * height * 4];  // zero = transparent black
        ulong uuid = InternalCalls.Texture_CreateRuntime(width, height, pixels, (int)filter);
        if (uuid == 0)
        {
            Log.Error("Texture.Create: native texture creation failed (WebGPU not ready?).");
            return null;
        }
        return new Texture(uuid, width, height, pixels);
    }

    public bool IsValid => UUID != 0 && (IsRuntime || InternalCalls.Texture_LoadAsset(UUID));

    public string Name
    {
        get
        {
            if (UUID == 0)
                return AssetDisplay.NoneLabel;
            if (IsRuntime)
                return "<runtime>";

            string name = InternalCalls.Asset_GetDisplayName(UUID);
            return string.IsNullOrEmpty(name) ? "(Missing Asset)" : name;
        }
    }

    public string Path => (UUID != 0 && !IsRuntime) ? InternalCalls.Asset_GetPath(UUID) : "";
    public int Width => IsRuntime ? _runtimeWidth : (UUID != 0 ? InternalCalls.Texture_GetWidth(UUID) : 0);
    public int Height => IsRuntime ? _runtimeHeight : (UUID != 0 ? InternalCalls.Texture_GetHeight(UUID) : 0);

    // ── Per-pixel access (runtime textures only) ────────────────────────
    // All of these operate on the managed CPU buffer; nothing reaches the GPU
    // until Apply(). GetPixel/SetPixel on an asset (file-backed) texture throw
    // — only Create()d textures own a readable/writable buffer.

    /// <summary>Set one pixel. (0,0) is top-left. Out-of-bounds cells are ignored.</summary>
    public void SetPixel(Vector2Int cell, Color color)
    {
        if (_pixels == null) throw new InvalidOperationException(
            "SetPixel is only valid on a texture created with Texture.Create.");
        if (cell.X < 0 || cell.Y < 0 || cell.X >= _runtimeWidth || cell.Y >= _runtimeHeight) return;
        int i = (cell.Y * _runtimeWidth + cell.X) * 4;
        _pixels[i + 0] = ToByte(color.R);
        _pixels[i + 1] = ToByte(color.G);
        _pixels[i + 2] = ToByte(color.B);
        _pixels[i + 3] = ToByte(color.A);
    }

    /// <summary>Read one pixel back from the CPU buffer. (0,0) is top-left.</summary>
    public Color GetPixel(Vector2Int cell)
    {
        if (_pixels == null) throw new InvalidOperationException(
            "GetPixel is only valid on a texture created with Texture.Create.");
        if (cell.X < 0 || cell.Y < 0 || cell.X >= _runtimeWidth || cell.Y >= _runtimeHeight)
            return new Color(0, 0, 0, 0);
        int i = (cell.Y * _runtimeWidth + cell.X) * 4;
        return new Color(_pixels[i] / 255f, _pixels[i + 1] / 255f, _pixels[i + 2] / 255f, _pixels[i + 3] / 255f);
    }

    /// <summary>
    /// Bulk-set every pixel row-major (length must be Width*Height). Much
    /// faster than per-pixel SetPixel for full-texture writes.
    /// </summary>
    public void SetPixels(Color[] colors)
    {
        if (_pixels == null) throw new InvalidOperationException(
            "SetPixels is only valid on a texture created with Texture.Create.");
        int count = _runtimeWidth * _runtimeHeight;
        if (colors.Length != count)
            throw new ArgumentException($"SetPixels expected {count} colors, got {colors.Length}.");
        for (int p = 0; p < count; p++)
        {
            int i = p * 4;
            _pixels[i + 0] = ToByte(colors[p].R);
            _pixels[i + 1] = ToByte(colors[p].G);
            _pixels[i + 2] = ToByte(colors[p].B);
            _pixels[i + 3] = ToByte(colors[p].A);
        }
    }

    /// <summary>Upload all pending CPU-buffer changes to the GPU. Cheap to call
    /// once per batch of edits; the whole image is re-uploaded each call.</summary>
    public void Apply()
    {
        if (_pixels == null) return;  // asset textures are immutable from script
        InternalCalls.Texture_UpdateRuntime(UUID, _pixels);
    }

    public void Dispose()
    {
        if (_pixels != null && UUID != 0)
        {
            InternalCalls.Texture_DestroyRuntime(UUID);
            _pixels = null;
            UUID = 0;
        }
    }

    private static byte ToByte(float v) => (byte)Math.Clamp((int)(v * 255f + 0.5f), 0, 255);

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
