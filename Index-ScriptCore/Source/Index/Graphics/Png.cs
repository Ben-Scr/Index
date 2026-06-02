using System.Buffers.Binary;
using System.IO;
using System.IO.Compression;

namespace Index.Graphics;

/// <summary>
/// Minimal, dependency-free PNG encoder for 8-bit RGBA images. Uses only the
/// .NET base class library (System.IO.Compression for the zlib/DEFLATE stream),
/// so it behaves identically in the editor and in shipped builds with no extra
/// packages.
///
/// Pixels are RGBA8, row-major, top-left origin (row 0 is the top), 4 bytes per
/// pixel — the same layout as a runtime <see cref="Texture"/>'s CPU buffer, so
/// <see cref="Texture.SaveToPng"/> feeds straight into this.
/// </summary>
public static class Png
{
    private static readonly byte[] s_Signature = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

    /// <summary>Encode an RGBA8 image and write it to <paramref name="path"/>, creating the parent folder if needed.</summary>
    /// <param name="path">Destination file path (absolute, or relative to the process working directory).</param>
    /// <param name="width">Image width in pixels (&gt; 0).</param>
    /// <param name="height">Image height in pixels (&gt; 0).</param>
    /// <param name="rgba">Pixel bytes; length must be &gt;= width*height*4.</param>
    public static void Save(string path, int width, int height, ReadOnlySpan<byte> rgba)
    {
        byte[] png = Encode(width, height, rgba);

        string? directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
            Directory.CreateDirectory(directory);

        File.WriteAllBytes(path, png);
    }

    /// <summary>Encode an RGBA8 image to an in-memory PNG byte array.</summary>
    public static byte[] Encode(int width, int height, ReadOnlySpan<byte> rgba)
    {
        if (width <= 0 || height <= 0)
            throw new ArgumentException($"Png.Encode: invalid size {width}x{height}.");

        long required = (long)width * height * 4;
        if (rgba.Length < required)
            throw new ArgumentException(
                $"Png.Encode: pixel buffer too small — need {required} bytes for {width}x{height} RGBA, got {rgba.Length}.");

        using var output = new MemoryStream();
        output.Write(s_Signature, 0, s_Signature.Length);

        Span<byte> ihdr = stackalloc byte[13];
        BinaryPrimitives.WriteUInt32BigEndian(ihdr[..4], (uint)width);
        BinaryPrimitives.WriteUInt32BigEndian(ihdr[4..8], (uint)height);
        ihdr[8] = 8;   // bit depth
        ihdr[9] = 6;   // color type: truecolor + alpha (RGBA)
        ihdr[10] = 0;  // compression method: DEFLATE
        ihdr[11] = 0;  // filter method: standard (per-scanline filter byte)
        ihdr[12] = 0;  // interlace: none
        WriteChunk(output, "IHDR", ihdr);

        WriteChunk(output, "IDAT", Compress(width, height, rgba));
        WriteChunk(output, "IEND", ReadOnlySpan<byte>.Empty);

        return output.ToArray();
    }

    // zlib-wrapped DEFLATE of the raw scanlines, each prefixed with filter byte 0 (None).
    private static byte[] Compress(int width, int height, ReadOnlySpan<byte> rgba)
    {
        int stride = width * 4;
        using var compressed = new MemoryStream();
        using (var zlib = new ZLibStream(compressed, CompressionLevel.Optimal, leaveOpen: true))
        {
            for (int y = 0; y < height; y++)
            {
                zlib.WriteByte(0); // filter type: None
                zlib.Write(rgba.Slice(y * stride, stride));
            }
        }
        return compressed.ToArray();
    }

    private static void WriteChunk(Stream stream, string type, ReadOnlySpan<byte> data)
    {
        Span<byte> length = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(length, (uint)data.Length);
        stream.Write(length);

        Span<byte> typeBytes = stackalloc byte[4];
        typeBytes[0] = (byte)type[0];
        typeBytes[1] = (byte)type[1];
        typeBytes[2] = (byte)type[2];
        typeBytes[3] = (byte)type[3];
        stream.Write(typeBytes);
        stream.Write(data);

        // PNG chunk CRC is computed over the type bytes followed by the data.
        uint crc = 0xFFFFFFFFu;
        crc = Crc32Update(crc, typeBytes);
        crc = Crc32Update(crc, data);
        crc ^= 0xFFFFFFFFu;

        Span<byte> crcBytes = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(crcBytes, crc);
        stream.Write(crcBytes);
    }

    private static readonly uint[] s_CrcTable = BuildCrcTable();

    private static uint[] BuildCrcTable()
    {
        var table = new uint[256];
        for (uint n = 0; n < 256; n++)
        {
            uint c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        return table;
    }

    private static uint Crc32Update(uint crc, ReadOnlySpan<byte> data)
    {
        foreach (byte b in data)
            crc = s_CrcTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc;
    }
}
