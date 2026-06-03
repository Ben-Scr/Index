using System;
using Index.Graphics;
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

    // Runtime/procedural CPU buffers, keyed by synthetic UUID. The native GPU
    // texture is already shared by UUID, so we mirror that: ANY managed Texture
    // wrapper for a given runtime UUID (your Create() handle OR one returned by
    // SpriteRenderer.Texture) resolves to the SAME pixel buffer here. That's
    // why `sprite.Texture.SetPixel(...)` works without holding the original
    // handle. Main-thread only (scripts never touch this concurrently).
    private sealed class RuntimeBuffer { public byte[] Pixels = System.Array.Empty<byte>(); public int Width; public int Height; public int OutOfBoundsWarnings; }
    private static readonly System.Collections.Generic.Dictionary<ulong, RuntimeBuffer> s_RuntimeBuffers = new();

    /// <summary>True for a script-created (procedural) texture — it owns a CPU
    /// pixel buffer and must be Dispose()d to free its GPU resource.</summary>
    public bool IsRuntime => s_RuntimeBuffers.ContainsKey(UUID);

    internal Texture(ulong assetId)
    {
        UUID = assetId;
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
        s_RuntimeBuffers[uuid] = new RuntimeBuffer { Pixels = pixels, Width = width, Height = height };
        return new Texture(uuid);
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
    public int Width => s_RuntimeBuffers.TryGetValue(UUID, out var rb) ? rb.Width : (UUID != 0 ? InternalCalls.Texture_GetWidth(UUID) : 0);
    public int Height => s_RuntimeBuffers.TryGetValue(UUID, out var rb) ? rb.Height : (UUID != 0 ? InternalCalls.Texture_GetHeight(UUID) : 0);

    // ── Per-pixel access (runtime textures only) ────────────────────────
    // All of these operate on the managed CPU buffer; nothing reaches the GPU
    // until Apply(). They work on ANY wrapper for a runtime UUID (including one
    // read back from SpriteRenderer.Texture), since the buffer is keyed by UUID.
    // GetPixel/SetPixel on an asset (file-backed) texture throw — only Create()d
    // textures own a buffer.

    private RuntimeBuffer RequireBuffer(string op)
    {
        if (!s_RuntimeBuffers.TryGetValue(UUID, out var rb))
            throw new InvalidOperationException(
                $"{op} is only valid on a texture created with Texture.Create (this one is a file/asset texture).");
        return rb;
    }

    // Out-of-bounds pixel access is almost always a bug, but SetPixel/GetPixel
    // are per-pixel APIs frequently called inside tight loops — logging every
    // miss would flood the editor log (which does NOT coalesce duplicate lines).
    // So warn, but cap the count per texture and note when the rest are dropped.
    private const int k_MaxOutOfBoundsWarnings = 8;

    private static void WarnOutOfBounds(RuntimeBuffer rb, string op, Vector2Int cell)
    {
        if (rb.OutOfBoundsWarnings >= k_MaxOutOfBoundsWarnings)
            return;
        rb.OutOfBoundsWarnings++;
        string suffix = rb.OutOfBoundsWarnings == k_MaxOutOfBoundsWarnings
            ? " (further out-of-bounds warnings for this texture are suppressed)"
            : "";
        Log.Warn(
            $"Texture.{op}: cell ({cell.X}, {cell.Y}) is outside the texture bounds {rb.Width}x{rb.Height} — " +
            $"valid range is (0, 0) to ({rb.Width - 1}, {rb.Height - 1}); pixel skipped.{suffix}");
    }

    /// <summary>Set one pixel. (0,0) is top-left. An out-of-bounds cell is
    /// skipped and logs a warning (capped per texture to avoid log spam).</summary>
    public void SetPixel(Vector2Int cell, Color color)
    {
        RuntimeBuffer rb = RequireBuffer(nameof(SetPixel));
        if (cell.X < 0 || cell.Y < 0 || cell.X >= rb.Width || cell.Y >= rb.Height)
        {
            WarnOutOfBounds(rb, nameof(SetPixel), cell);
            return;
        }
        int i = (cell.Y * rb.Width + cell.X) * 4;
        rb.Pixels[i + 0] = ToByte(color.R);
        rb.Pixels[i + 1] = ToByte(color.G);
        rb.Pixels[i + 2] = ToByte(color.B);
        rb.Pixels[i + 3] = ToByte(color.A);
    }

    /// <summary>Read one pixel back from the CPU buffer. (0,0) is top-left.
    /// An out-of-bounds cell returns transparent black and logs a warning
    /// (capped per texture to avoid log spam).</summary>
    public Color GetPixel(Vector2Int cell)
    {
        RuntimeBuffer rb = RequireBuffer(nameof(GetPixel));
        if (cell.X < 0 || cell.Y < 0 || cell.X >= rb.Width || cell.Y >= rb.Height)
        {
            WarnOutOfBounds(rb, nameof(GetPixel), cell);
            return new Color(0, 0, 0, 0);
        }
        int i = (cell.Y * rb.Width + cell.X) * 4;
        return new Color(rb.Pixels[i] / 255f, rb.Pixels[i + 1] / 255f, rb.Pixels[i + 2] / 255f, rb.Pixels[i + 3] / 255f);
    }

    /// <summary>
    /// Bulk-set every pixel row-major (length must be Width*Height). Much
    /// faster than per-pixel SetPixel for full-texture writes.
    /// </summary>
    public void SetPixels(Color[] colors)
    {
        RuntimeBuffer rb = RequireBuffer(nameof(SetPixels));
        int count = rb.Width * rb.Height;
        if (colors.Length != count)
            throw new ArgumentException($"SetPixels expected {count} colors, got {colors.Length}.");
        for (int p = 0; p < count; p++)
        {
            int i = p * 4;
            rb.Pixels[i + 0] = ToByte(colors[p].R);
            rb.Pixels[i + 1] = ToByte(colors[p].G);
            rb.Pixels[i + 2] = ToByte(colors[p].B);
            rb.Pixels[i + 3] = ToByte(colors[p].A);
        }
    }

    /// <summary>
    /// Bulk-read every pixel row-major (length Width*Height; (0,0) is top-left)
    /// — the inverse of <see cref="SetPixels"/>, and faster than calling
    /// <see cref="GetPixel"/> per cell. The returned array is a fresh copy of
    /// the CPU buffer; mutating it does not change the texture (write it back
    /// with <see cref="SetPixels"/> + <see cref="Apply"/>). Only valid on a
    /// runtime texture created with <see cref="Create"/>.
    /// </summary>
    public Color[] GetPixels()
    {
        RuntimeBuffer rb = RequireBuffer(nameof(GetPixels));
        int count = rb.Width * rb.Height;
        Color[] colors = new Color[count];
        for (int p = 0; p < count; p++)
        {
            int i = p * 4;
            colors[p] = new Color(
                rb.Pixels[i + 0] / 255f,
                rb.Pixels[i + 1] / 255f,
                rb.Pixels[i + 2] / 255f,
                rb.Pixels[i + 3] / 255f);
        }
        return colors;
    }

    /// <summary>Upload all pending CPU-buffer changes to the GPU. Cheap to call
    /// once per batch of edits; the whole image is re-uploaded each call.</summary>
    public void Apply()
    {
        if (s_RuntimeBuffers.TryGetValue(UUID, out var rb))
            InternalCalls.Texture_UpdateRuntime(UUID, rb.Pixels);
        // asset textures are immutable from script — silently no-op
    }

    /// <summary>
    /// Write this texture to a PNG file. Only valid for runtime textures created
    /// with <see cref="Create"/> (their pixels live in a CPU buffer). The current
    /// CPU buffer is saved as-is, so you do NOT need to call <see cref="Apply"/>
    /// first — Apply only pushes to the GPU. For a file/asset texture the source
    /// image is already on disk; copy <see cref="Path"/> instead.
    /// </summary>
    /// <param name="path">Destination .png path (absolute, or relative to the working directory); the parent folder is created if missing.</param>
    /// <returns>true on success; false if this isn't a runtime texture or the write failed.</returns>
    public bool SaveToPng(string path)
    {
        if (!s_RuntimeBuffers.TryGetValue(UUID, out var rb))
        {
            Log.Error("Texture.SaveToPng is only valid on a runtime texture created with Texture.Create. " +
                      "A file/asset texture is already on disk — copy Texture.Path instead.");
            return false;
        }

        try
        {
            Png.Save(path, rb.Width, rb.Height, rb.Pixels);
            return true;
        }
        catch (Exception e)
        {
            Log.Error($"Texture.SaveToPng failed for '{path}': {e.Message}");
            return false;
        }
    }

    public void Dispose()
    {
        if (s_RuntimeBuffers.Remove(UUID) && UUID != 0)
        {
            InternalCalls.Texture_DestroyRuntime(UUID);
        }
        UUID = 0;
    }

    private static byte ToByte(float v) => (byte)Math.Clamp((int)(v * 255f + 0.5f), 0, 255);

    // Public so packages built in separate assemblies (Pkg.<Name>.dll)
    // can construct managed Texture wrappers from a stored asset GUID
    // without re-implementing the AssetRegistry lookup.
    public static Texture? FromAssetUUID(ulong assetId)
    {
        if (assetId == 0)
            return null;

        // Runtime/procedural textures aren't in the AssetRegistry, so the
        // file-asset Asset_IsValid check would reject them. They resolve
        // through the texture manager instead. The returned wrapper is
        // reference-only (no CPU buffer) — keep the original Texture.Create
        // handle if you need GetPixel/SetPixel/Apply on it.
        if (InternalCalls.Texture_IsRuntime(assetId))
            return new Texture(assetId);

        if (!InternalCalls.Asset_IsValid(assetId))
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
