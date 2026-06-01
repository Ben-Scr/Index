using System;
using System.Collections.Concurrent;
using System.Runtime.ExceptionServices;

namespace Index.Jobs.Internal;

// Captures exceptions thrown on native worker threads so JobHandle.Complete can rethrow them on the calling thread. Keyed by native handle ID; only the first exception per handle is kept (mirrors Parallel.ForEach).
internal static class JobExceptionRegistry
{
    private static readonly ConcurrentDictionary<ulong, Exception> s_Stash = new();

    internal static void Stash(ulong nativeHandle, Exception ex)
    {
        if (nativeHandle == 0 || ex == null) return;
        // TryAdd preserves the first exception when multiple batches race.
        s_Stash.TryAdd(nativeHandle, ex);
    }

    internal static void RethrowIfPresent(ulong nativeHandle)
    {
        if (nativeHandle == 0) return;
        if (s_Stash.TryRemove(nativeHandle, out Exception? ex) && ex != null)
        {
            ExceptionDispatchInfo.Capture(ex).Throw();
        }
    }

    // Called from the release callback. Logs and clears any stashed
    // exception that nobody consumed via Complete().
    internal static void DrainOrLog(ulong nativeHandle)
    {
        if (nativeHandle == 0) return;
        if (s_Stash.TryRemove(nativeHandle, out Exception? ex) && ex != null)
        {
            try { Log.Error($"[Job] Unobserved exception: {ex}"); }
            catch { Console.Error.WriteLine($"[Job] Unobserved exception: {ex}"); }
        }
    }
}

