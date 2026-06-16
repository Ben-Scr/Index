using System;
using Index.Interop;

namespace Index;

/// <summary>
/// Logging interface that forwards messages to the native Index logging system (spdlog).
/// </summary>
public static class Log
{
    public static event Action<string>? LogMessage;

    // The public LogMessage event runs user code, and Log.* is called from inside
    // the catch block of every native→managed entry point. A throwing subscriber
    // here would unwind back across the reverse-pinvoke boundary and crash the
    // process — defeating the very guards that call us. Swallow it and report
    // straight to native (which does NOT re-raise this event, so no recursion).
    internal static void RaiseLogMessage(string message)
    {
        Action<string>? handler = LogMessage;
        if (handler == null) return;
        try { handler(message); }
        catch (Exception ex) { InternalCalls.Log_Error($"Log.LogMessage subscriber threw: {ex}"); }
    }

    private static void Write(string message, Action<string> nativeWrite)
    {
        nativeWrite(message);
        RaiseLogMessage(message);
    }

    internal static void Print(object? obj) => Log.Info(obj?.ToString() ?? "null");

    public static void Trace(string message) => Write(message, InternalCalls.Log_Trace);
    public static void Trace(object obj) => Trace(obj?.ToString() ?? "null");

    public static void Info(string message) => Write(message, InternalCalls.Log_Info);
    public static void Info(object obj) => Info(obj?.ToString() ?? "null");

    public static void Warn(string message) => Write(message, InternalCalls.Log_Warn);
    public static void Warn(object obj) => Warn(obj?.ToString() ?? "null");

    public static void Error(string message) => Write(message, InternalCalls.Log_Error);
    public static void Error(object obj) => Error(obj?.ToString() ?? "null");
}
