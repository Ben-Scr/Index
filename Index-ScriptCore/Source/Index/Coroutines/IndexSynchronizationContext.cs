using System.Collections.Generic;
using System.Threading;

namespace Index.Coroutines;

/// <summary>SynchronizationContext installed at ScriptHostBridge.Initialize; Post is thread-safe, Pump runs on the engine main thread (called from CoroutineScheduler.PumpUpdate).</summary>
internal sealed class IndexSynchronizationContext : SynchronizationContext
{
    public static readonly IndexSynchronizationContext Instance = new();

    private readonly object m_Lock = new();
    private Queue<Continuation> m_Queue = new();

    private readonly struct Continuation
    {
        public readonly SendOrPostCallback Callback;
        public readonly object? State;
        public Continuation(SendOrPostCallback c, object? s) { Callback = c; State = s; }
    }

    public override void Post(SendOrPostCallback d, object? state)
    {
        lock (m_Lock) { m_Queue.Enqueue(new Continuation(d, state)); }
    }

    public override void Send(SendOrPostCallback d, object? state)
    {
        // Engine is single-threaded; Send is not used. Run inline so a
        // misbehaving caller still completes rather than deadlocking.
        d(state);
    }

    /// <summary>Drain queued continuations on the engine main thread; OperationCanceledException is swallowed (expected from cancelled async void).</summary>
    public void Pump()
    {
        Queue<Continuation> batch;
        lock (m_Lock)
        {
            if (m_Queue.Count == 0) return;
            batch = m_Queue;
            m_Queue = new Queue<Continuation>();
        }

        while (batch.TryDequeue(out var c))
        {
            try
            {
                c.Callback(c.State);
            }
            catch (System.OperationCanceledException)
            {
                // expected for cancelled awaits in async void
            }
            catch (System.Exception ex)
            {
                Log.Error($"[Coroutine] Unhandled exception in continuation: {ex}");
            }
        }
    }
}
