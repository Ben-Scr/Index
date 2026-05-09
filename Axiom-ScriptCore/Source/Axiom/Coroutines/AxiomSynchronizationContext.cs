using System.Collections.Generic;
using System.Threading;

namespace Axiom.Coroutines;

/// <summary>
/// SynchronizationContext that pumps continuations on the engine main
/// thread. Installed once during ScriptHostBridge.Initialize so any async
/// continuation captured inside a script resumes inside the engine's
/// per-frame pump rather than on a thread-pool thread.
///
/// Threading: Post may be called from any thread (e.g. Task continuations
/// on the thread pool). Pump always runs on the engine main thread, called
/// from CoroutineScheduler.PumpUpdate.
/// </summary>
internal sealed class AxiomSynchronizationContext : SynchronizationContext
{
    public static readonly AxiomSynchronizationContext Instance = new();

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

    /// <summary>
    /// Drain queued continuations on the calling thread (engine main).
    /// Each callback is wrapped in a try/catch — OperationCanceledException
    /// is the expected outcome when a cancelled `async void` reports its
    /// exception via AsyncVoidMethodBuilder, so we silently swallow it.
    /// </summary>
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
