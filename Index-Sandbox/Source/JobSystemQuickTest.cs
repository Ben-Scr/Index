using System;
using System.Collections.Generic;
using System.Threading;
using Index;
using Index.Jobs;

// Quicktest for the IJob / IJobFor / IJobParallelFor scheduling paths in
// Index.Jobs. Drop this system onto any scene (no entities required). It
// runs on OnStart, exercises sequential and parallel schedules, atomics,
// reducers, dependencies, and cancellation, and logs PASS/FAIL per case
// followed by a summary line.
public class JobSystemQuickTest : GameSystem
{
    [ShowInEditor("Run on Start")] public bool RunOnStart = true;

    private int m_TotalCases;
    private int m_PassedCases;

    public override void OnStart()
    {
        if (!RunOnStart) return;
        RunAllCases();
    }

    // ── Test job structs ─────────────────────────────────────────

    // IJob (single-shot): bumps a shared counter so Complete-then-assert
    // is observable. AtomicInt is a ref type so the struct copy still
    // sees the same object.
    private struct FlipBoolJob : IJob
    {
        public AtomicInt Flag;
        public void Execute() => Flag.Store(1);
    }

    // IJobFor (sequential): stamps the array slot with index+1. Captures
    // an Order container so we can assert in-order visitation.
    private struct StampSequentialJob : IJobFor
    {
        public int[] Visited;
        public OrderTracker Order;
        public void Execute(int index)
        {
            Visited[index] = index + 1;
            if (Order.Expected != index) Order.HasViolation = true;
            Order.Expected = index + 1;
        }
    }

    // IJobParallelFor: every index must be visited exactly once. Increment
    // the per-slot count with Interlocked and bump a shared AtomicInt to
    // cross-check the total dispatch count.
    private struct ParallelVisitJob : IJobParallelFor
    {
        public int[] Counts;
        public AtomicInt Total;
        public void Execute(int index)
        {
            Interlocked.Increment(ref Counts[index]);
            Total.Increment();
        }
    }

    // IJobParallelFor: feeds a thread-local reducer. Doc demo for the
    // "use ParallelReducer instead of a hot atomic" pattern.
    private struct SumReducerJob : IJobParallelFor
    {
        public ParallelReducer<int> Reducer;
        public void Execute(int index) => Reducer.Add(index);
    }

    // IJob, used in the dependency-chain case as the producer.
    private struct WriteValueJob : IJob
    {
        public AtomicInt Value;
        public int ToWrite;
        public void Execute() => Value.Store(ToWrite);
    }

    // IJob, used as the consumer that observes the producer's write.
    private struct ReadValueJob : IJob
    {
        public AtomicInt Value;
        public AtomicInt Observed;
        public void Execute()
        {
            Observed.Store(Value.Load());
            Value.Increment();
        }
    }

    // IJobFor with a small sleep per iteration so cancellation has
    // something to interrupt. The sleep is intentionally cheap so the
    // test stays fast in the happy path.
    private struct SleepJob : IJobFor
    {
        public void Execute(int index) => Thread.Sleep(10);
    }

    // IJobParallelFor that is also schedulable sequentially via the
    // IJobFor overload — exercises the inheritance design.
    private struct DualSchedulingJob : IJobParallelFor
    {
        public AtomicInt Counter;
        public void Execute(int index) => Counter.Increment();
    }

    // IJobParallelFor that exercises ParallelRandom from worker threads.
    // If the random facade were sharing one xorshift state across workers,
    // unsynchronized writes would routinely zero or degenerate the state
    // and the output array would show massive duplication / runs of 0.
    private struct ParallelRandomJob : IJobParallelFor
    {
        public int[] Outputs;
        public void Execute(int index) => Outputs[index] = ParallelRandom.NextInt();
    }

    // Ref-type sidecar so the sequential StampJob's order state survives
    // the per-task struct copy. Plain int fields on the struct would not.
    private sealed class OrderTracker
    {
        public int Expected;
        public bool HasViolation;
    }

    // ── Driver ──────────────────────────────────────────────────

    private void RunAllCases()
    {
        m_TotalCases = 0;
        m_PassedCases = 0;

        Case("IJob runs and completes",                       Case_IJobRuns);
        Case("IJobFor sequential ordering",                   Case_IJobForSequential);
        Case("IJobParallelFor visits each index exactly once", Case_IJobParallelForVisitOnce);
        Case("ParallelReducer.SumInt sums 0..N-1",            Case_ReducerSum);
        Case("JobHandle dependency chains writes correctly",  Case_DependencyChain);
        Case("Cancellation propagates through JobHandle.Complete", Case_Cancellation);
        Case("IJobParallelFor marker also accepted by sequential Schedule", Case_DualScheduling);
        Case("ParallelRandom is thread-safe across parallel workers",      Case_ParallelRandomThreadSafe);

        Log.Info($"[JobSystemQuickTest] {m_PassedCases}/{m_TotalCases} cases passed");
    }

    // ── Cases ───────────────────────────────────────────────────

    private void Case_IJobRuns()
    {
        var flag = new AtomicInt(0);
        JobHandle h = Job.Schedule(new FlipBoolJob { Flag = flag });
        h.Complete();

        if (flag.Load() != 1)
            throw new InvalidOperationException($"IJob did not run: flag={flag.Load()}, expected 1");
    }

    private void Case_IJobForSequential()
    {
        const int N = 64;
        var visited = new int[N];
        var order = new OrderTracker();

        JobHandle h = Job.Schedule(new StampSequentialJob { Visited = visited, Order = order }, N);
        h.Complete();

        if (order.HasViolation)
            throw new InvalidOperationException("Sequential IJobFor visited indices out of order");
        if (order.Expected != N)
            throw new InvalidOperationException($"Sequential IJobFor visited {order.Expected} indices, expected {N}");

        for (int i = 0; i < N; i++)
        {
            if (visited[i] != i + 1)
                throw new InvalidOperationException($"Sequential IJobFor: slot {i} has {visited[i]}, expected {i + 1}");
        }
    }

    private void Case_IJobParallelForVisitOnce()
    {
        const int N = 4096;
        var counts = new int[N];
        var total = new AtomicInt(0);

        JobHandle h = Job.ScheduleParallelFor(new ParallelVisitJob { Counts = counts, Total = total }, N);
        h.Complete();

        if (total.Load() != N)
            throw new InvalidOperationException($"Parallel total: got {total.Load()}, expected {N}");

        for (int i = 0; i < N; i++)
        {
            if (counts[i] != 1)
                throw new InvalidOperationException($"Parallel visit: slot {i} has count {counts[i]}, expected 1");
        }
    }

    private void Case_ReducerSum()
    {
        const int N = 10_000;
        using var reducer = new ParallelReducer<int>(0, static (a, b) => a + b);

        JobHandle h = Job.ScheduleParallelFor(new SumReducerJob { Reducer = reducer }, N);
        h.Complete();

        long expected = (long)N * (N - 1) / 2;
        long actual = reducer.Result();
        if (actual != expected)
            throw new InvalidOperationException($"ParallelReducer sum: got {actual}, expected {expected}");
    }

    private void Case_DependencyChain()
    {
        var value = new AtomicInt(0);
        var observed = new AtomicInt(-1);

        JobHandle a = Job.Schedule(new WriteValueJob { Value = value, ToWrite = 7 });
        JobHandle b = Job.Schedule(new ReadValueJob { Value = value, Observed = observed }, a);
        b.Complete();

        if (observed.Load() != 7)
            throw new InvalidOperationException($"Dependency chain: observed={observed.Load()}, expected 7 (producer write should have happened-before consumer)");
        if (value.Load() != 8)
            throw new InvalidOperationException($"Dependency chain: value={value.Load()}, expected 8 (consumer should have bumped 7)");
    }

    private void Case_Cancellation()
    {
        using var cts = new CancellationTokenSource();
        // length × sleep = ~5 seconds worst case; cancel fires at 20ms.
        JobHandle h = Job.Schedule(new SleepJob(), 500, cts.Token);
        cts.CancelAfter(20);

        ExpectThrows<OperationCanceledException>(() => h.Complete(),
            "JobHandle.Complete must propagate OperationCanceledException when the token trips mid-flight");
    }

    private void Case_DualScheduling()
    {
        const int N = 256;
        var counter = new AtomicInt(0);

        // Sequential — works because IJobParallelFor : IJobFor.
        JobHandle hSeq = Job.Schedule(new DualSchedulingJob { Counter = counter }, N);
        hSeq.Complete();
        if (counter.Load() != N)
            throw new InvalidOperationException($"Dual sequential: counter={counter.Load()}, expected {N}");

        // Parallel — works because the constraint is IJobFor (and our struct
        // also satisfies IJobParallelFor explicitly).
        counter.Store(0);
        JobHandle hPar = Job.ScheduleParallelFor(new DualSchedulingJob { Counter = counter }, N);
        hPar.Complete();
        if (counter.Load() != N)
            throw new InvalidOperationException($"Dual parallel: counter={counter.Load()}, expected {N}");
    }

    private void Case_ParallelRandomThreadSafe()
    {
        const int N = 10_000;
        var outputs = new int[N];

        JobHandle h = Job.ScheduleParallelFor(new ParallelRandomJob { Outputs = outputs }, N);
        h.Complete();

        // Distinctness: with [ThreadStatic] state, ~10k 31-bit positives should
        // have effectively zero birthday collisions (expected ≈ 0.023). A race
        // on shared state would either zero the xorshift cycle (lots of 0s) or
        // collapse threads onto the same sub-sequence (massive duplication).
        var seen = new HashSet<int>(N);
        int zeros = 0;
        for (int i = 0; i < N; i++)
        {
            if (outputs[i] == 0) zeros++;
            seen.Add(outputs[i]);
        }

        if (zeros > 5)
            throw new InvalidOperationException($"ParallelRandom produced {zeros} zero outputs out of {N} — possible xorshift state degeneration from a race");
        if (seen.Count < N - 50)
            throw new InvalidOperationException($"ParallelRandom produced only {seen.Count} distinct outputs out of {N} — too many duplicates to be chance");

        // Generator advances: a second parallel run must produce a different
        // sequence (per-thread state was not reset between runs).
        var outputs2 = new int[N];
        JobHandle h2 = Job.ScheduleParallelFor(new ParallelRandomJob { Outputs = outputs2 }, N);
        h2.Complete();

        bool identical = true;
        for (int i = 0; i < N; i++)
        {
            if (outputs[i] != outputs2[i]) { identical = false; break; }
        }
        if (identical)
            throw new InvalidOperationException("ParallelRandom produced identical sequences across two parallel runs — per-thread generator did not advance");
    }

    // ── Harness helpers ─────────────────────────────────────────

    private void Case(string name, Action body)
    {
        m_TotalCases++;
        try
        {
            body();
            m_PassedCases++;
            Log.Info($"[JobSystemQuickTest] PASS  {name}");
        }
        catch (Exception ex)
        {
            Log.Error($"[JobSystemQuickTest] FAIL  {name}: {ex.Message}");
        }
    }

    private static void ExpectThrows<T>(Action body, string message) where T : Exception
    {
        try
        {
            body();
        }
        catch (T)
        {
            return;
        }
        catch (Exception ex)
        {
            // AggregateException can wrap the cancellation when the underlying
            // task observes it through Wait/GetResult; unwrap one level.
            if (ex is AggregateException agg && agg.GetBaseException() is T) return;
            throw new InvalidOperationException($"{message}: caught {ex.GetType().Name} instead of {typeof(T).Name}: {ex.Message}");
        }
        throw new InvalidOperationException($"{message}: no exception thrown");
    }
}
