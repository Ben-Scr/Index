using System;
using System.IO;

namespace Index;

public static class Application
{
    public static event Action? OnPaused;
    public static event Action? OnStart;
    public static event Action? OnQuit;

    internal static void RaiseApplicationPaused() => OnPaused?.Invoke();
    internal static void RaiseApplicationStart() => OnStart?.Invoke();
    internal static void RaiseApplicationQuit() => OnQuit?.Invoke();

    public static float TargetFrameRate
    {
        get => InternalCalls.Application_GetTargetFrameRate();
        set => InternalCalls.Application_SetTargetFrameRate(value);
    }

    public static bool VsyncEnabled
    {
        get => InternalCalls.Application_GetVsyncEnabled();
        set => InternalCalls.Application_SetVsyncEnabled(value);
    }

    /// <summary>Writable persistent data directory (save files, settings). Guaranteed to exist.</summary>
    public static string PersistentDataPath => EnsureDirectory(
        Path.Combine(GetLocalApplicationDataPath(), "Index"));

    /// <summary>Absolute path to the current project's <c>Assets</c> folder (e.g. <c>…/MyGame/Assets</c>); empty if no project is loaded.</summary>
    public static string DataPath => InternalCalls.Application_GetDataPath();

    /// <summary>
    /// The temporary cache directory. Data here may be cleared by the OS.
    /// </summary>
    public static string TemporaryCachePath => EnsureDirectory(
        Path.Combine(Path.GetTempPath(), "Index"));

    public static int ScreenWidth => InternalCalls.Application_GetScreenWidth();
    public static int ScreenHeight => InternalCalls.Application_GetScreenHeight();
    public static float AspectRatio => ScreenHeight > 0 ? (float)ScreenWidth / ScreenHeight : 1.0f;

    public static bool RunningInBackground
    {
        get => InternalCalls.Application_GetRunInBackground();
        set => InternalCalls.Application_SetRunInBackground(value);
    }

    /// <summary>Runtime sibling of the compile-time INDEX_EDITOR define; use <c>#if INDEX_EDITOR</c> to strip at compile time, this property for runtime branches.</summary>
    public static bool IsEditor => InternalCalls.Application_IsEditor();

    /// <summary>Logical CPU core count; returns 4 as a floor when the OS can't report a value.</summary>
    public static int ProcessorCount => InternalCalls.Application_GetProcessorCount();

    /// <summary>OS clipboard string via GLFW; returns empty and setter is a no-op in headless (no window) contexts.</summary>
    public static string ClipboardString
    {
        get => InternalCalls.Application_GetClipboardString();
        set => InternalCalls.Application_SetClipboardString(value);
    }

    /// <summary>
    /// Quits the application. Only works in build mode, ignored in the editor.
    /// </summary>
    public static void Quit() => InternalCalls.Application_Quit();
    public static void Reload() => InternalCalls.Application_Reload();

    private static string GetLocalApplicationDataPath()
    {
        string path = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        return string.IsNullOrEmpty(path) ? AppContext.BaseDirectory : path;
    }

    private static string EnsureDirectory(string path)
    {
        Directory.CreateDirectory(path);
        return path;
    }
}
