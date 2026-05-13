using Index.Interop;
using System;
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics.X86;
using System.Text;

namespace Index;

/// <summary>
/// Build configuration the script is running under.
/// Mirrors the INDEX_BUILD_* preprocessor symbols emitted at script-compile time.
/// </summary>
public enum BuildConfiguration
{
    /// <summary>Editor preview / iteration mode.</summary>
    Debug = 0,
    /// <summary>Built game with diagnostics enabled (INDEX_BUILD_DEVELOPMENT).</summary>
    Development = 1,
    /// <summary>Shipped game build (INDEX_BUILD_RELEASE).</summary>
    Release = 2,
}

public enum Platform
{
    Unknown = 0,
    Windows,
    Linux,
    MacOS,
    Android,
    iOS,
    WebGL
}

public enum GraphicsApi
{
    Unknown = 0,
    Direct3D11,
    Direct3D12,
    Vulkan,
    Metal,
    OpenGL,
    OpenGLES,
    WebGPU
}

/// <summary>
/// Engine-level identity and graphics-capability info.
/// Read-only; values are determined at process start.
/// </summary>
public static class Engine
{
    /// <summary>Engine semver string, e.g. "2026.1.0".</summary>
    public static string Version => InternalCalls.Engine_GetVersion();

    /// <summary>Full engine version string, e.g. "Index 2026.1.0 (Windows x64 Release)".
    /// Mirrors the IDX_VERSION_LONG macro on the C++ side.</summary>
    public static string VersionLong => InternalCalls.Engine_GetVersionLong();
    public static string ConfigurationName => BuildConfiguration.ToString();

    /// <summary>
    /// Active build configuration (Debug / Development / Release). Mirrors
    /// the INDEX_BUILD_* defines but resolved at runtime so callers don't
    /// need #if directives.
    /// </summary>
    public static BuildConfiguration BuildConfiguration
        => (BuildConfiguration)InternalCalls.Engine_GetBuildConfiguration();

    /// <summary>Current operating system.</summary>
    public static Index.Platform Platform => ParsePlatform(PlatformName);
    public static string PlatformName => InternalCalls.Engine_GetPlatform();

    /// <summary>
    /// Active graphics backend enum. Use GraphicsApiName for the raw backend label.
    /// </summary>
    public static Index.GraphicsApi GraphicsApi => ParseGraphicsApi(GraphicsApiName);
    public static string GraphicsApiName => InternalCalls.Engine_GetGraphicsApi();

    /// <summary>GPU vendor name, e.g. "NVIDIA", "AMD", "Intel".</summary>
    public static string GpuVendor => InternalCalls.Engine_GetGpuVendor();
    public static string CpuVendor => QueryCpuVendor();

    /// <summary>Active renderer backend name, e.g. "Vulkan", "Direct3D12", "Metal".</summary>
    public static string GpuRenderer => InternalCalls.Engine_GetGpuRenderer();

    private static Index.Platform ParsePlatform(string value)
        => value switch
        {
            "Windows" => Index.Platform.Windows,
            "Linux" => Index.Platform.Linux,
            "MacOS" or "macOS" or "OSX" => Index.Platform.MacOS,
            "Android" => Index.Platform.Android,
            "iOS" => Index.Platform.iOS,
            "WebGL" => Index.Platform.WebGL,
            _ => Index.Platform.Unknown
        };

    private static Index.GraphicsApi ParseGraphicsApi(string value)
    {
        if (string.IsNullOrWhiteSpace(value)) return Index.GraphicsApi.Unknown;
        if (value.Contains("D3D11", StringComparison.OrdinalIgnoreCase)
            || value.Contains("Direct3D11", StringComparison.OrdinalIgnoreCase)) return Index.GraphicsApi.Direct3D11;
        if (value.Contains("D3D12", StringComparison.OrdinalIgnoreCase)
            || value.Contains("Direct3D12", StringComparison.OrdinalIgnoreCase)) return Index.GraphicsApi.Direct3D12;
        if (value.Contains("Vulkan", StringComparison.OrdinalIgnoreCase)) return Index.GraphicsApi.Vulkan;
        if (value.Contains("Metal", StringComparison.OrdinalIgnoreCase)) return Index.GraphicsApi.Metal;
        if (value.Contains("OpenGLES", StringComparison.OrdinalIgnoreCase)) return Index.GraphicsApi.OpenGLES;
        if (value.Contains("OpenGL", StringComparison.OrdinalIgnoreCase)) return Index.GraphicsApi.OpenGL;
        if (value.Contains("WebGPU", StringComparison.OrdinalIgnoreCase)) return Index.GraphicsApi.WebGPU;
        return Index.GraphicsApi.Unknown;
    }

    private static string QueryCpuVendor()
    {
        if (X86Base.IsSupported)
        {
            (int _, int ebx, int ecx, int edx) = X86Base.CpuId(0, 0);
            Span<byte> bytes = stackalloc byte[12];
            BitConverter.TryWriteBytes(bytes[0..4], ebx);
            BitConverter.TryWriteBytes(bytes[4..8], edx);
            BitConverter.TryWriteBytes(bytes[8..12], ecx);
            string vendor = Encoding.ASCII.GetString(bytes).TrimEnd('\0', ' ');
            if (!string.IsNullOrWhiteSpace(vendor)) return vendor;
        }

        return RuntimeInformation.ProcessArchitecture.ToString();
    }
}
