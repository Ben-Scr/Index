using System;
using System.Collections.Generic;
using System.Threading;

namespace Index.Coroutines;

/// <summary>
/// Owns the queues of pending coroutine continuations. Pumped each frame
/// by ScriptSystem.Update / FixedUpdate via two managed callbacks wired
/// in ScriptHostBridge.Initialize.
///
/// Cancellation: each entry holds a CancellationToken captured from its
/// owning EntityScript's destroy CTS. When the script is destroyed the
/// token cancels — the next pump tick fires the continuation, GetResult
/// throws OperationCanceledException, and the user's async state machine
/// unwinds. No closures retain the script after that point, so the user
/// AssemblyLoadContext can unload on hot reload.
///
/// Thread safety: Register may come from a thread-pool thread when a
/// user awaits Task.Run().ConfigureAwait(false); pump always runs on the
/// engine main thread. A single lock protects all four lists — contention
/// is negligible at game frame rates.
/// </summary>
internal static class CoroutineScheduler
{
    private sealed class FrameEntry
    {
        public Action Continuation = null!;
        public CancellationToken Token;
        public int FramesRemaining;
    }

    private sealed class SecondsEntry
    {
        public Action Continuation = null!;
        public CancellationToken Token;
        public float SecondsRemaining;
    }

    private sealed class ConditionEntry
    {
        public Action Continuation = null!;
        public CancellationToken Token;
        public Func<bool> Predicate = null!;
    }

    private sealed class FixedEntry
    {
        public Action Continuation = null!;
        public CancellationToken Token;
    }

    private static readonly object s_Lock = new();
    private static List<FrameEntry> s_FrameEntries = new();
    private static List<SecondsEntry> s_SecondsEntries = new();
    private static List<ConditionEntry> s_ConditionEntries = new();
    private static List<FixedEntry> s_FixedEntries = new();

    public static void RegisterFrame(Action continuation, CancellationToken token, int framesRemaining)
    {
        if (framesRemaining < 1) framesRemaining = 1;
        lock (s_Lock)
        {
            s_FrameEntries.Add(new FrameEntry
            {
                Continuation = continuation,
                Token = token,
                FramesRemaining = framesRemaining,
            });
        }
    }

    public static void RegisterSeconds(Action continuation, CancellationToken token, float seconds)
    {
        if (seconds < 0f) seconds = 0f;
        lock (s_Lock)
        {
            s_SecondsEntries.Add(new SecondsEntry
            {
                Continuation = continuation,
                Token = token,
                SecondsRemaining = seconds,
            });
        }
    }

    public static void RegisterCondition(Action continuation, CancellationToken token, Func<bool> predicate)
    {
        lock (s_Lock)
        {
            s_ConditionEntries.Add(new ConditionEntry
            {
                Continuation = continuation,
                Token = token,
                Predicate = predicate,
            });
        }
    }

    public static void RegisterFixedUpdate(Action continuation, CancellationToken token)
    {
        lock (s_Lock)
        {
            s_FixedEntries.Add(new FixedEntry
            {
                Continuation = continuation,
                Token = token,
            });
        }
    }

    /// <summary>
    /// Drains the sync-context queue, then ticks frame / seconds / condition
    /// entries, then drains the sync-context queue again so async void
    /// exceptions thrown by continuations we just ran are logged this
    /// frame instead of bleeding into next.
    /// </summary>
    public static void PumpUpdate(float deltaTime)
    {
        IndexSynchronizationContext.Instance.Pump();

        List<FrameEntry> frame;
        List<SecondsEntry> seconds;
        List<ConditionEntry> conditions;
        lock (s_Lock)
        {
            frame = s_FrameEntries;
            seconds = s_SecondsEntries;
            conditions = s_ConditionEntries;
            s_FrameEntries = new List<FrameEntry>();
            s_SecondsEntries = new List<SecondsEntry>();
            s_ConditionEntries = new List<ConditionEntry>();
        }

        ProcessFrameEntries(frame);
        ProcessSecondsEntries(seconds, deltaTime);
        ProcessConditionEntries(conditions);

        IndexSynchronizationContext.Instance.Pump();
    }

    public static void PumpFixedUpdate()
    {
        List<FixedEntry> fixedEntries;
        lock (s_Lock)
        {
            fixedEntries = s_FixedEntries;
            s_FixedEntries = new List<FixedEntry>();
        }

        foreach (var entry in fixedEntries)
        {
            FireOrCancel(entry.Continuation);
        }
    }

    /// <summary>
    /// Drop every pending entry without firing. Called from
    /// ScriptInstanceManager.UnloadUserAssembly during hot reload — the
    /// owning AssemblyLoadContext is about to unload so any captured user
    /// types must be released for the GC dance to succeed.
    /// </summary>
    public static void Reset()
    {
        lock (s_Lock)
        {
            s_FrameEntries.Clear();
            s_SecondsEntries.Clear();
            s_ConditionEntries.Clear();
            s_FixedEntries.Clear();
        }
    }

    private static void ProcessFrameEntries(List<FrameEntry> entries)
    {
        List<FrameEntry>? keepers = null;
        foreach (var entry in entries)
        {
            if (entry.Token.IsCancellationRequested)
            {
                FireOrCancel(entry.Continuation);
                continue;
            }

            entry.FramesRemaining--;
            if (entry.FramesRemaining <= 0)
            {
                FireOrCancel(entry.Continuation);
            }
            else
            {
                keepers ??= new List<FrameEntry>();
                keepers.Add(entry);
            }
        }
        if (keepers != null)
        {
            lock (s_Lock) { s_FrameEntries.AddRange(keepers); }
        }
    }

    private static void ProcessSecondsEntries(List<SecondsEntry> entries, float deltaTime)
    {
        List<SecondsEntry>? keepers = null;
        foreach (var entry in entries)
        {
            if (entry.Token.IsCancellationRequested)
            {
                FireOrCancel(entry.Continuation);
                continue;
            }

            entry.SecondsRemaining -= deltaTime;
            if (entry.SecondsRemaining <= 0f)
            {
                FireOrCancel(entry.Continuation);
            }
            else
            {
                keepers ??= new List<SecondsEntry>();
                keepers.Add(entry);
            }
        }
        if (keepers != null)
        {
            lock (s_Lock) { s_SecondsEntries.AddRange(keepers); }
        }
    }

    private static void ProcessConditionEntries(List<ConditionEntry> entries)
    {
        List<ConditionEntry>? keepers = null;
        foreach (var entry in entries)
        {
            if (entry.Token.IsCancellationRequested)
            {
                FireOrCancel(entry.Continuation);
                continue;
            }

            bool predicateResult;
            try { predicateResult = entry.Predicate(); }
            catch (Exception ex)
            {
                // A broken predicate must not loop forever; fire the
                // continuation so the await unwinds with the predicate
                // exception (caught by AsyncVoidMethodBuilder for async
                // void, or stored on the Task for async Task).
                Log.Error($"[Coroutine] WaitUntil predicate threw: {ex}");
                FireOrCancel(entry.Continuation);
                continue;
            }

            if (predicateResult)
            {
                FireOrCancel(entry.Continuation);
            }
            else
            {
                keepers ??= new List<ConditionEntry>();
                keepers.Add(entry);
            }
        }
        if (keepers != null)
        {
            lock (s_Lock) { s_ConditionEntries.AddRange(keepers); }
        }
    }

    private static void FireOrCancel(Action continuation)
    {
        try
        {
            continuation();
        }
        catch (OperationCanceledException) { }
        catch (Exception ex)
        {
            Log.Error($"[Coroutine] Continuation threw: {ex}");
        }
    }
}
