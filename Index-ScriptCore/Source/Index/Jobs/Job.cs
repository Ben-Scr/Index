using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using Index.Interop;
using Index.Jobs.Internal;

namespace Index.Jobs;

/// <summary>
/// Single-shot asynchronous work unit. Scheduled with
/// <see cref="Job.Schedule{TJob}(TJob, System.Threading.CancellationToken)"/> — the
/// runtime spins up one task that calls <see cref="Execute"/> once.
/// </summary>
public interface IJob
{
    void Execute();
}

/// <summary>
/// Indexed work unit. Two scheduling shapes are supported:
/// <list type="bullet">
///   <item><see cref="Job.Schedule{TJob}(TJob, int, System.Threading.CancellationToken)"/> —
///   sequential, single-worker, in-order from <c>0..length-1</c>.</item>
///   <item><see cref="Job.ScheduleParallelFor{TJob}(TJob, int, int, System.Threading.CancellationToken)"/> —
///   parallel across workers when <see cref="Execute"/> is thread-safe; prefer
///   declaring the struct as <see cref="IJobParallelFor"/> in that case so the
///   intent is visible at the call site.</item>
/// </list>
/// </summary>
public interface IJobFor
{
    void Execute(int index);
}

/// <summary>
/// Marker interface declaring that <see cref="IJobFor.Execute"/> is safe to
/// call concurrently across worker threads. The job struct is copied per
/// partition, so any shared mutable state must use <c>AtomicInt</c> /
/// <c>AtomicLong</c> / <c>AtomicFloat</c> / <c>ParallelReducer&lt;T&gt;</c>
/// rather than plain fields. Inherits from <see cref="IJobFor"/> so the same
/// struct can also be scheduled sequentially when desired.
/// </summary>
public interface IJobParallelFor : IJobFor
{
}

public readonly struct JobSystemSpec
{
    public JobSystemSpec(int workerCount = -1)
    {
        WorkerCount = workerCount;
    }

    public int WorkerCount { get; }
}

public readonly struct JobHandle
{
    // Dual-mode handle: typed Schedule paths (IJob / IJobFor /
    // IJobParallelFor) route through the native pool and carry only a
    // ulong handle ID. Action-based and dependency-chained paths still
    // run on the .NET ThreadPool and carry a Task. Exactly one of the
    // two is set per construction. Both fields default to zero/null so
    // a default-constructed JobHandle is "nothing to do, already done".
    private readonly Task? m_Task;
    private readonly ulong m_NativeHandle;

    internal JobHandle(Task? task)
    {
        m_Task = task;
        m_NativeHandle = 0;
    }

    internal JobHandle(ulong nativeHandle)
    {
        m_Task = null;
        m_NativeHandle = nativeHandle;
    }

    internal Task? InnerTask => m_Task;
    internal ulong NativeHandle => m_NativeHandle;

    public bool IsValid => m_Task != null || m_NativeHandle != 0;

    public unsafe bool IsComplete
    {
        get
        {
            if (m_NativeHandle != 0)
            {
                var fn = NativeCallbacks.Bindings.JobSystem_IsComplete;
                return fn == null || fn(m_NativeHandle) != 0;
            }
            return m_Task == null || m_Task.IsCompleted;
        }
    }

    public bool IsCompleted => IsComplete;
    public bool IsFaulted => m_Task?.IsFaulted ?? false;
    public bool IsCanceled => m_Task?.IsCanceled ?? false;
    public Exception? Exception => m_Task?.Exception?.GetBaseException();

    public void Complete() => Job.Wait(this);

    public Task AsTask()
    {
        if (m_Task != null) return m_Task;
        if (m_NativeHandle == 0) return Task.CompletedTask;
        // Native-handle shim: wrap the blocking Wait in a Task so
        // `await` / ContinueWith callers still work. This is a fallback
        // for back-compat — typed Schedule callers should prefer
        // Complete() to avoid the extra Task allocation.
        ulong nh = m_NativeHandle;
        return Task.Run(() => Job.WaitNativeHandle(nh));
    }

    public TaskAwaiter GetAwaiter() => AsTask().GetAwaiter();

    /// <summary>
    /// Opt in to fire-and-forget fault logging. Awaited handles
    /// re-throw naturally and don't need this. Native-handle paths
    /// surface unconsumed exceptions from the release callback on the
    /// engine log automatically, so this is a no-op for those — kept
    /// for API symmetry with the Task-backed handles.
    /// </summary>
    public JobHandle ObserveFaults()
    {
        if (m_Task != null) Job.AttachFaultLogger(m_Task);
        return this;
    }
}

public static class JobSystem
{
    private static int s_WorkerCount = ResolveWorkerCount(-1);

    public static bool IsInitialized => true;
    public static int WorkerCount
    {
        get
        {
            // Prefer the native pool's worker count when the binding is
            // available — the native side is the source of truth for how
            // many threads typed Schedule<TJob> calls fan out across.
            // Falls back to the managed cache during very early init
            // before the host bridge has populated the bindings table.
            unsafe
            {
                var fn = NativeCallbacks.Bindings.JobSystem_GetWorkerCount;
                if (fn != null) return fn();
            }
            return Volatile.Read(ref s_WorkerCount);
        }
    }
    public static bool IsCallerWorker => Thread.CurrentThread.IsThreadPoolThread;
    public static int WorkerIndex => -1;

    public static void Initialize(JobSystemSpec spec = default)
    {
        Configure(spec.WorkerCount == 0 ? -1 : spec.WorkerCount);
    }

    public static void Configure(int workerCount = -1)
    {
        // Resize the native pool first so the managed cache mirrors
        // whatever the engine actually settled on (the native side
        // applies its own clamps and INDEX_WITH_SCRIPTING headroom rules
        // for the auto path). When the native binding isn't yet wired,
        // resolve managed-side and let the next native init catch up.
        int resolved;
        unsafe
        {
            var fn = NativeCallbacks.Bindings.JobSystem_Reconfigure;
            resolved = fn != null ? fn(workerCount) : ResolveWorkerCount(workerCount);
        }
        Volatile.Write(ref s_WorkerCount, resolved);

        // Invalidate the cached default ParallelOptions so RunParallelFor
        // rebuilds with the new degree of parallelism on the next call.
        Job.InvalidateDefaultOptions();

        // CLR ThreadPool only handles the .NET-Task fallback paths now —
        // typed Schedule<TJob> / ScheduleParallelFor<TJob> /
        // ScheduleParallelForRange dispatch through the native
        // work-stealing pool. A small min (2) covers Action overloads,
        // dependency continuations, and AsTask() shims without
        // oversubscribing cores against the native pool.
        const int kManagedMinThreads = 2;
        ThreadPool.GetMinThreads(out int minWorkerThreads, out int minCompletionPortThreads);
        if (minWorkerThreads < kManagedMinThreads)
        {
            ThreadPool.SetMinThreads(kManagedMinThreads, minCompletionPortThreads);
        }
    }

    public static void Shutdown()
    {
    }

    public static int GetWorkerCount() => WorkerCount;
    public static bool IsCallerWorkerThread() => IsCallerWorker;
    public static int GetWorkerIndex() => WorkerIndex;

    internal static int ComputeAutoBatchSize(int rangeLength)
    {
        if (rangeLength <= 0)
        {
            return 1;
        }

        int targetChunks = Math.Max(1, WorkerCount * 4);
        int batchSize = rangeLength / targetChunks;
        return batchSize > 0 ? batchSize : 1;
    }

    private static int ResolveWorkerCount(int requested)
    {
        if (requested > 0)
        {
            return Math.Clamp(requested, 1, 32);
        }

        int processorCount = Environment.ProcessorCount;
        if (processorCount <= 0)
        {
            processorCount = 4;
        }

        int workerCount = processorCount - 1;
        if (workerCount < 1)
        {
            workerCount = 1;
        }
        if (workerCount > 16)
        {
            workerCount = 16;
        }
        return workerCount;
    }
}

public static class Job
{
    public static JobHandle Schedule(Action work, CancellationToken cancellationToken = default)
        => Schedule(work, default, cancellationToken);

    public static JobHandle Schedule(Action work, JobHandle dependency, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(work);
        return ToHandle(StartScheduled(work, dependency.InnerTask, cancellationToken));
    }

    public static JobHandle Schedule<TJob>(TJob job, CancellationToken cancellationToken = default)
        where TJob : struct, IJob
        => Schedule(job, default, cancellationToken);

    public static JobHandle Schedule<TJob>(TJob job, JobHandle dependency, CancellationToken cancellationToken = default)
        where TJob : struct, IJob
    {
        Internal.JobLayoutCache.EnsureValidated<TJob>();

        Internal.JobBox<TJob> box = Internal.JobBoxPool<TJob>.Rent();
        box.Job = job;
        box.Token = cancellationToken;

        // Dependency present? Stay on the .NET Task path so we can
        // ContinueWith. The native pool has no dependency primitive;
        // forcing one would mean either spinning a worker on Wait or
        // building managed-side fan-in logic. .NET handles this
        // cleanly already.
        if (dependency.IsValid)
        {
            return ToHandle(StartScheduled(box.ExecuteAction, dependency.InnerTask, cancellationToken));
        }

        // Fast path: no dependency → dispatch through native pool. The
        // box's Invoke() runs once on a native worker, OnRelease()
        // returns the box to the per-thread pool when the handle is
        // released.
        return Internal.JobNativeDispatch.Schedule(box);
    }

    /// <summary>
    /// Schedules <paramref name="job"/> to run <see cref="IJobFor.Execute"/> sequentially
    /// for indices <c>0..length-1</c> on a single worker. The job struct is copied
    /// before the loop; mutations to <typeparamref name="TJob"/> fields are not
    /// visible to the caller. For concurrent dispatch across workers use
    /// <see cref="ScheduleParallelFor{TJob}(TJob, int, int, CancellationToken)"/>.
    /// </summary>
    public static JobHandle Schedule<TJob>(
        TJob job,
        int length,
        CancellationToken cancellationToken = default)
        where TJob : struct, IJobFor
        => Schedule(job, length, default, cancellationToken);

    /// <summary>
    /// Sequential <see cref="IJobFor"/> schedule overload with an explicit dependency.
    /// The loop body waits for <paramref name="dependency"/> to complete before
    /// running indices <c>0..length-1</c> in order.
    /// </summary>
    public static JobHandle Schedule<TJob>(
        TJob job,
        int length,
        JobHandle dependency,
        CancellationToken cancellationToken = default)
        where TJob : struct, IJobFor
    {
        Internal.JobLayoutCache.EnsureValidated<TJob>();

        if (length < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(length));
        }

        if (length == 0)
        {
            return default;
        }

        Internal.JobForBox<TJob> box = Internal.JobForBoxPool<TJob>.Rent();
        box.Job = job;
        box.Token = cancellationToken;
        box.Length = length;

        if (dependency.IsValid)
        {
            return ToHandle(StartScheduled(box.ExecuteAction, dependency.InnerTask, cancellationToken));
        }

        // Sequential IJobFor is a single-shot from the scheduler's
        // point of view (one worker runs the whole loop) so it goes
        // through the same native Enqueue path as IJob.
        return Internal.JobNativeDispatch.Schedule(box);
    }

    public static JobHandle ScheduleParallelFor(
        int length,
        Action<int> body,
        int batchSize = 0,
        CancellationToken cancellationToken = default)
        => ScheduleParallelFor(0, length, body, batchSize, default, cancellationToken);

    public static JobHandle ScheduleParallelFor(
        int length,
        Action<int> body,
        int batchSize,
        JobHandle dependency,
        CancellationToken cancellationToken = default)
        => ScheduleParallelFor(0, length, body, batchSize, dependency, cancellationToken);

    public static JobHandle ScheduleParallelFor(
        int begin,
        int end,
        Action<int> body,
        int batchSize = 0,
        CancellationToken cancellationToken = default)
        => ScheduleParallelFor(begin, end, body, batchSize, default, cancellationToken);

    public static JobHandle ScheduleParallelFor(
        int begin,
        int end,
        Action<int> body,
        int batchSize,
        JobHandle dependency,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(body);
        ValidateRange(begin, end);

        if (begin == end)
        {
            return default;
        }

        return ToHandle(StartScheduled(
            () => RunParallelFor(begin, end, batchSize, body, cancellationToken),
            dependency.InnerTask,
            cancellationToken));
    }

    /// <summary>
    /// Schedules <paramref name="job"/> for parallel indexed execution across worker
    /// threads. Indices in <c>0..length-1</c> are partitioned into batches and
    /// dispatched concurrently; the job struct is copied per partition. The
    /// constraint is <see cref="IJobFor"/> for flexibility, but authors should
    /// mark concurrency-safe jobs with <see cref="IJobParallelFor"/> so the
    /// intent is visible at the declaration site, and use <c>AtomicInt</c> /
    /// <c>ParallelReducer&lt;T&gt;</c> for shared mutable state.
    /// </summary>
    public static JobHandle ScheduleParallelFor<TJob>(
        TJob job,
        int length,
        int batchSize = 0,
        CancellationToken cancellationToken = default)
        where TJob : struct, IJobFor
        => ScheduleParallelFor(job, length, batchSize, default, cancellationToken);

    /// <summary>
    /// Parallel <see cref="IJobFor"/> schedule overload with an explicit dependency.
    /// The parallel dispatch waits for <paramref name="dependency"/> to complete
    /// before partitioning begins.
    /// </summary>
    public static JobHandle ScheduleParallelFor<TJob>(
        TJob job,
        int length,
        int batchSize,
        JobHandle dependency,
        CancellationToken cancellationToken = default)
        where TJob : struct, IJobFor
    {
        Internal.JobLayoutCache.EnsureValidated<TJob>();

        if (length < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(length));
        }

        if (length == 0)
        {
            return default;
        }

        Internal.JobParallelForBox<TJob> box = Internal.JobParallelForBoxPool<TJob>.Rent();
        box.Job = job;
        box.Token = cancellationToken;
        box.Begin = 0;
        box.End = length;
        box.BatchSize = batchSize;

        if (dependency.IsValid)
        {
            // Dependency: keep on the .NET Task path so we can
            // ContinueWith. The box's Run() method falls back to
            // RunParallelForFromBox (Parallel.ForEach) for this case.
            return ToHandle(StartScheduled(box.ExecuteAction, dependency.InnerTask, cancellationToken));
        }

        // Native fan-out — JobSystem_ParallelFor partitions [0, length)
        // into chunks and dispatches each chunk through the native
        // work-stealing pool. Each chunk callback fires
        // box.InvokeRange(lo, hi) on a native worker.
        return Internal.JobNativeDispatch.ScheduleParallelFor(box, 0, length, batchSize);
    }

    // Called from JobParallelForBox<TJob>.Run — keeps the partitioned
    // dispatch in this file while letting the box hold the state.
    internal static void RunParallelForFromBox<TJob>(Internal.JobParallelForBox<TJob> box)
        where TJob : struct, IJobFor
    {
        RunParallelFor(box.Begin, box.End, box.BatchSize, box.Job, box.Token);
    }

    /// <summary>
    /// Parallel range-body variant of <see cref="ScheduleParallelFor(int, Action{int}, int, CancellationToken)"/>.
    /// The body receives the partitioner's <c>[lo, hi)</c> range once
    /// per batch, so callers can amortize per-index overhead (loop
    /// variable capture, function-pointer dispatch) across the batch.
    /// Use when the per-index work is small enough that the
    /// <see cref="Action{int}"/> dispatch dominates.
    /// </summary>
    public static JobHandle ScheduleParallelForRange(
        int begin,
        int end,
        Action<int, int> rangeBody,
        int batchSize = 0,
        CancellationToken cancellationToken = default)
        => ScheduleParallelForRange(begin, end, rangeBody, batchSize, default, cancellationToken);

    public static JobHandle ScheduleParallelForRange(
        int begin,
        int end,
        Action<int, int> rangeBody,
        int batchSize,
        JobHandle dependency,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(rangeBody);
        ValidateRange(begin, end);

        if (begin == end)
        {
            return default;
        }

        if (dependency.IsValid)
        {
            return ToHandle(StartScheduled(
                () => RunParallelForRange(begin, end, batchSize, rangeBody, cancellationToken),
                dependency.InnerTask,
                cancellationToken));
        }

        // Route through native parallel-for so the partitioned dispatch
        // runs on the engine work-stealing pool instead of the CLR
        // ThreadPool. Each chunk fires `box.InvokeRange(lo, hi)` which
        // invokes the user's rangeBody.
        Internal.RangeBodyBox box = Internal.RangeBodyBox.Rent(rangeBody, cancellationToken);
        return Internal.JobNativeDispatch.ScheduleParallelFor(box, begin, end, batchSize);
    }

    public static void ParallelForRange(int begin, int end, Action<int, int> rangeBody, int batchSize = 0)
        => ScheduleParallelForRange(begin, end, rangeBody, batchSize).Complete();

    public static void ParallelFor(int length, Action<int> body, int batchSize = 0)
        => ScheduleParallelFor(length, body, batchSize).Complete();

    public static void ParallelFor(int begin, int end, Action<int> body, int batchSize = 0)
        => ScheduleParallelFor(begin, end, body, batchSize).Complete();

    public static void ParallelFor<TJob>(TJob job, int length, int batchSize = 0)
        where TJob : struct, IJobFor
        => ScheduleParallelFor(job, length, batchSize).Complete();

    /// <summary>
    /// Synchronously runs <see cref="IJobFor.Execute"/> for indices
    /// <c>0..length-1</c> on a single worker thread. Equivalent to
    /// <c>Job.Schedule(job, length).Complete()</c>.
    /// </summary>
    public static void RunFor<TJob>(TJob job, int length)
        where TJob : struct, IJobFor
        => Schedule(job, length).Complete();

    public static unsafe void Wait(JobHandle handle)
    {
        ulong nh = handle.NativeHandle;
        if (nh != 0)
        {
            WaitNativeHandle(nh);
            return;
        }
        handle.InnerTask?.GetAwaiter().GetResult();
    }

    // Internal helper shared by JobHandle.AsTask and Wait. Blocks until
    // the native handle completes, then rethrows any stashed exception
    // on the calling thread (so the contract matches Task.Wait —
    // exceptions surface as InvalidOperation/JobExecutionException
    // rather than vanishing into the native worker).
    internal static unsafe void WaitNativeHandle(ulong nativeHandle)
    {
        if (nativeHandle == 0) return;
        ref NativeBindingsStruct b = ref NativeCallbacks.Bindings;
        if (b.JobSystem_Wait != null)
        {
            b.JobSystem_Wait(nativeHandle);
        }
        try
        {
            JobExceptionRegistry.RethrowIfPresent(nativeHandle);
        }
        finally
        {
            if (b.JobSystem_Release != null)
            {
                b.JobSystem_Release(nativeHandle);
            }
        }
    }

    public static bool IsComplete(JobHandle handle) => handle.IsComplete;

    public static JobHandle CombineDependencies(params JobHandle[] handles)
    {
        ArgumentNullException.ThrowIfNull(handles);

        List<Task>? tasks = null;
        foreach (JobHandle handle in handles)
        {
            Task? task = handle.InnerTask;
            if (task == null)
            {
                continue;
            }

            tasks ??= new List<Task>();
            tasks.Add(task);
        }

        return tasks == null || tasks.Count == 0
            ? default
            : ToHandle(Task.WhenAll(tasks));
    }

    public static JobHandle CombineDependencies(IEnumerable<JobHandle> handles)
    {
        ArgumentNullException.ThrowIfNull(handles);

        List<Task>? tasks = null;
        foreach (JobHandle handle in handles)
        {
            Task? task = handle.InnerTask;
            if (task == null)
            {
                continue;
            }

            tasks ??= new List<Task>();
            tasks.Add(task);
        }

        return tasks == null || tasks.Count == 0
            ? default
            : ToHandle(Task.WhenAll(tasks));
    }

    public static void WaitAll(params JobHandle[] handles)
    {
        CombineDependencies(handles).Complete();
    }

    private static Task StartScheduled(Action run, Task? dependency, CancellationToken cancellationToken)
    {
        // Fast path: no dependency and no cancelable token — hand `run`
        // straight to the scheduler. Avoids the wrapper closure that
        // would otherwise capture (dependency, cancellationToken, run).
        // The box's Run() method handles its own cancellation check
        // before invoking user code.
        if (dependency == null && !cancellationToken.CanBeCanceled)
        {
            return Task.Factory.StartNew(
                run,
                CancellationToken.None,
                TaskCreationOptions.DenyChildAttach,
                TaskScheduler.Default);
        }

        if (dependency == null || dependency.IsCompleted)
        {
            return Task.Factory.StartNew(
                () =>
                {
                    dependency?.GetAwaiter().GetResult();
                    cancellationToken.ThrowIfCancellationRequested();
                    run();
                },
                cancellationToken,
                TaskCreationOptions.DenyChildAttach,
                TaskScheduler.Default);
        }

        return dependency.ContinueWith(
            completedDependency =>
            {
                completedDependency.GetAwaiter().GetResult();
                cancellationToken.ThrowIfCancellationRequested();
                run();
            },
            CancellationToken.None,
            TaskContinuationOptions.DenyChildAttach,
            TaskScheduler.Default);
    }

    private static JobHandle ToHandle(Task task)
    {
        // Previously this attached a fault-logging ContinueWith on every
        // Schedule, costing one Task allocation per call even when the
        // caller awaited the handle (and would naturally rethrow). We
        // now require fire-and-forget callers to opt in via
        // JobHandle.ObserveFaults() — awaited handles surface the
        // exception themselves through Complete / GetAwaiter.
        return new JobHandle(task);
    }

    internal static void AttachFaultLogger(Task task)
    {
        _ = task.ContinueWith(
            faultedTask =>
            {
                Exception? exception = faultedTask.Exception?.GetBaseException();
                if (exception == null)
                {
                    return;
                }

                try
                {
                    Log.Error($"[Job] {exception}");
                }
                catch
                {
                    Console.Error.WriteLine($"[Job] {exception}");
                }
            },
            CancellationToken.None,
            TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }

    private static void RunParallelFor(int begin, int end, int batchSize, Action<int> body, CancellationToken cancellationToken)
    {
        int resolvedBatchSize = ResolveBatchSize(begin, end, batchSize);
        ParallelOptions options = GetParallelOptions(cancellationToken);

        Parallel.ForEach(
            Partitioner.Create(begin, end, resolvedBatchSize),
            options,
            range =>
            {
                // The outer Parallel.ForEach polls the token between
                // partition entries, so an inner per-iteration check is
                // redundant — and costs ~1–2 ns × N. Cancellation
                // latency rises from per-index to per-batch
                // (~length/(workers*4) indices in auto-batch mode),
                // which is fine for sub-millisecond response.
                for (int i = range.Item1; i < range.Item2; ++i)
                {
                    body(i);
                }
            });
    }

    private static void RunParallelFor<TJob>(int begin, int end, int batchSize, TJob job, CancellationToken cancellationToken)
        where TJob : struct, IJobFor
    {
        int resolvedBatchSize = ResolveBatchSize(begin, end, batchSize);
        ParallelOptions options = GetParallelOptions(cancellationToken);

        Parallel.ForEach(
            Partitioner.Create(begin, end, resolvedBatchSize),
            options,
            range =>
            {
                TJob localJob = job;
                for (int i = range.Item1; i < range.Item2; ++i)
                {
                    localJob.Execute(i);
                }
            });
    }

    private static void RunParallelForRange(int begin, int end, int batchSize, Action<int, int> rangeBody, CancellationToken cancellationToken)
    {
        int resolvedBatchSize = ResolveBatchSize(begin, end, batchSize);
        ParallelOptions options = GetParallelOptions(cancellationToken);

        Parallel.ForEach(
            Partitioner.Create(begin, end, resolvedBatchSize),
            options,
            range => rangeBody(range.Item1, range.Item2));
    }

    // Cache the default-token ParallelOptions instance so the common
    // case (no caller-supplied CancellationToken) doesn't allocate per
    // call. Rebuilt only when JobSystem.WorkerCount changes (signalled
    // via InvalidateDefaultOptions from JobSystem.Configure).
    private static ParallelOptions? s_DefaultOptions;

    private static ParallelOptions GetParallelOptions(CancellationToken cancellationToken)
    {
        if (cancellationToken.CanBeCanceled)
        {
            // Per-call allocation only when a real token is supplied.
            // Cancellation-aware paths are uncommon on the hot loop.
            return new ParallelOptions
            {
                CancellationToken = cancellationToken,
                MaxDegreeOfParallelism = JobSystem.WorkerCount,
            };
        }

        ParallelOptions? cached = s_DefaultOptions;
        int wc = JobSystem.WorkerCount;
        if (cached == null || cached.MaxDegreeOfParallelism != wc)
        {
            // Racy under concurrent Configure() — losing thread rebuilds
            // and overwrites; both instances are valid so no harm done.
            cached = new ParallelOptions { MaxDegreeOfParallelism = wc };
            s_DefaultOptions = cached;
        }
        return cached;
    }

    internal static void InvalidateDefaultOptions()
    {
        s_DefaultOptions = null;
    }

    private static int ResolveBatchSize(int begin, int end, int batchSize)
    {
        if (batchSize > 0)
        {
            return batchSize;
        }

        return JobSystem.ComputeAutoBatchSize(end - begin);
    }

    private static void ValidateRange(int begin, int end)
    {
        if (end < begin)
        {
            throw new ArgumentOutOfRangeException(nameof(end), "End must be greater than or equal to begin.");
        }
    }
}

public static class ParallelFor
{
    public static JobHandle Schedule(int length, Action<int> body, int batchSize = 0, CancellationToken cancellationToken = default)
        => Job.ScheduleParallelFor(length, body, batchSize, cancellationToken);

    public static JobHandle Schedule(int length, Action<int> body, int batchSize, JobHandle dependency, CancellationToken cancellationToken = default)
        => Job.ScheduleParallelFor(length, body, batchSize, dependency, cancellationToken);

    public static JobHandle Schedule(int begin, int end, Action<int> body, int batchSize = 0, CancellationToken cancellationToken = default)
        => Job.ScheduleParallelFor(begin, end, body, batchSize, cancellationToken);

    public static JobHandle Schedule(int begin, int end, Action<int> body, int batchSize, JobHandle dependency, CancellationToken cancellationToken = default)
        => Job.ScheduleParallelFor(begin, end, body, batchSize, dependency, cancellationToken);

    public static JobHandle Schedule<TJob>(TJob job, int length, int batchSize = 0, CancellationToken cancellationToken = default)
        where TJob : struct, IJobFor
        => Job.ScheduleParallelFor(job, length, batchSize, cancellationToken);

    public static JobHandle Schedule<TJob>(TJob job, int length, int batchSize, JobHandle dependency, CancellationToken cancellationToken = default)
        where TJob : struct, IJobFor
        => Job.ScheduleParallelFor(job, length, batchSize, dependency, cancellationToken);

    public static void Run(int length, Action<int> body, int batchSize = 0)
        => Job.ParallelFor(length, body, batchSize);

    public static void Run(int begin, int end, Action<int> body, int batchSize = 0)
        => Job.ParallelFor(begin, end, body, batchSize);

    public static void Run<TJob>(TJob job, int length, int batchSize = 0)
        where TJob : struct, IJobFor
        => Job.ParallelFor(job, length, batchSize);
}
