using System;
using System.Collections.Concurrent;
using System.Runtime.ExceptionServices;

namespace Index.Jobs.Internal;

// Stashes exceptions thrown by managed job bodies (which run on a
// native worker thread, where we can't let them propagate into C++) so
// JobHandle.Complete can rethrow them on the calling thread later.
//
// Keyed by the native handle ID returned by JobSystem_Enqueue /
// JobSystem_ParallelFor — that ID is the only handle the calling code
// holds, so it's the natural lookup key.
//
// Lifecycle:
//   - Stash: called from the [UnmanagedCallersOnly] entry points when
//     user code throws.
//   - RethrowIfPresent: called from JobHandle.Complete before Release.
//     Removes and rethrows on the calling thread.
//   - StashIfNotConsumed: called from the release callback as a safety
//     net for fire-and-forget callers that never wait — surfaces the
//     exception via Log.Error so it doesn't vanish silently.
//
// For ParallelFor where multiple batches may stash separately, only
// the FIRST exception per handle is kept. Mirrors `Parallel.ForEach`
// behaviour where one batch's failure cancels the rest.
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
            // Rethrow on the calling thread preserving the original
            // exception type and stack trace. ExceptionDispatchInfo is
            // the canonical way to rethrow across a thread boundary
            // without wrapping (Throw() appends the rethrow site to
            // the existing stack instead of starting a new chain).
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

