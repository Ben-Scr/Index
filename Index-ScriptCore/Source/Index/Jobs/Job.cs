using System.Collections.Concurrent;
using System.Runtime.ExceptionServices;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;
using Index.Interop;
using Index.Jobs.Internal;

namespace Index.Jobs;

public interface IJob
{
    void Execute();
}

public interface IJobFor
{
    void Execute(int index);
}

// Shared mutable state must use AtomicInt/AtomicFloat/ParallelReducer — plain fields are not safe.
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
    // Dual-mode: typed Schedule paths carry shared native completion state;
    // Action/dependency paths carry a Task. Exactly one is set; default = already done.
    private readonly Task? m_Task;
    private readonly NativeJobState? m_NativeState;

    internal JobHandle(Task? task)
    {
        m_Task = task;
        m_NativeState = null;
    }

    internal JobHandle(ulong nativeHandle)
    {
        m_Task = null;
        m_NativeState = nativeHandle == 0 ? null : new NativeJobState(nativeHandle);
    }

    internal Task? InnerTask => m_Task;
    internal NativeJobState? NativeState => m_NativeState;
    internal ulong NativeHandle => m_NativeState?.Handle ?? 0;

    public bool IsValid => m_Task != null || m_NativeState != null;

    public bool IsComplete
    {
        get
        {
            if (m_NativeState != null) return m_NativeState.IsComplete;
            return m_Task == null || m_Task.IsCompleted;
        }
    }

    public bool IsCompleted => IsComplete;
    public bool IsFaulted => m_Task?.IsFaulted ?? m_NativeState?.IsFaulted ?? false;
    public bool IsCanceled => m_Task?.IsCanceled ?? false;
    public Exception? Exception => m_Task?.Exception?.GetBaseException() ?? m_NativeState?.Exception;

    public void Complete() => Job.Wait(this);

    public Task AsTask()
    {
        if (m_Task != null) return m_Task;
        return m_NativeState?.AsTask() ?? Task.CompletedTask;
    }

    public TaskAwaiter GetAwaiter() => AsTask().GetAwaiter();

    /// <summary>Opt in to fire-and-forget fault logging and native-handle cleanup.</summary>
    public JobHandle ObserveFaults()
    {
        if (IsValid) Job.AttachFaultLogger(AsTask());
        return this;
    }
}

internal sealed class NativeJobState
{
    private readonly ulong m_Handle;
    private readonly ManualResetEventSlim m_Completed = new(false);
    private readonly object m_TaskLock = new();
    private Task? m_Task;
    private ExceptionDispatchInfo? m_Exception;
    private int m_CompletionStarted;

    internal NativeJobState(ulong handle)
    {
        m_Handle = handle;
    }

    internal ulong Handle => m_Handle;
    internal bool IsFaulted => m_Exception != null;
    internal Exception? Exception => m_Exception?.SourceException;

    internal unsafe bool IsComplete
    {
        get
        {
            if (m_Completed.IsSet) return true;
            var fn = NativeCallbacks.Bindings.JobSystem_IsComplete;
            return fn == null || fn(m_Handle) != 0;
        }
    }

    internal void Complete()
    {
        if (Interlocked.CompareExchange(ref m_CompletionStarted, 1, 0) == 0)
        {
            try
            {
                Job.WaitNativeHandle(m_Handle);
            }
            catch (Exception ex)
            {
                m_Exception = ExceptionDispatchInfo.Capture(ex);
            }
            finally
            {
                m_Completed.Set();
            }
        }
        else
        {
            m_Completed.Wait();
        }

        m_Exception?.Throw();
    }

    internal Task AsTask()
    {
        lock (m_TaskLock)
        {
            return m_Task ??= Task.Run(Complete);
        }
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
            // Native binding is source of truth; fall back to managed cache during early init before the host bridge is ready.
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
        int resolved;
        unsafe
        {
            var fn = NativeCallbacks.Bindings.JobSystem_Reconfigure;
            resolved = fn != null ? fn(workerCount) : ResolveWorkerCount(workerCount);
        }
        Volatile.Write(ref s_WorkerCount, resolved);

        Job.InvalidateDefaultOptions();

        // Small CLR ThreadPool min (2) covers Action/dependency/shim paths without oversubscribing against the native work-stealing pool.
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
        return ToHandle(StartScheduled(work, GetDependencyTask(dependency), cancellationToken));
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

        // Native pool has no dependency primitive; stay on the Task path when a dependency is present.
        if (dependency.IsValid)
        {
            return ToHandle(StartScheduled(box.ExecuteAction, GetDependencyTask(dependency), cancellationToken));
        }

        return Internal.JobNativeDispatch.Schedule(box);
    }

    public static JobHandle Schedule<TJob>(
        TJob job,
        int length,
        CancellationToken cancellationToken = default)
        where TJob : struct, IJobFor
        => Schedule(job, length, default, cancellationToken);

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
            return ToHandle(StartScheduled(box.ExecuteAction, GetDependencyTask(dependency), cancellationToken));
        }

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
            GetDependencyTask(dependency),
            cancellationToken));
    }

    public static JobHandle ScheduleParallelFor<TJob>(
        TJob job,
        int length,
        int batchSize = 0,
        CancellationToken cancellationToken = default)
        where TJob : struct, IJobFor
        => ScheduleParallelFor(job, length, batchSize, default, cancellationToken);

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
            return ToHandle(StartScheduled(box.ExecuteAction, GetDependencyTask(dependency), cancellationToken));
        }

        return Internal.JobNativeDispatch.ScheduleParallelFor(box, 0, length, batchSize);
    }

    // Called from JobParallelForBox<TJob>.Run — keeps the partitioned
    // dispatch in this file while letting the box hold the state.
    internal static void RunParallelForFromBox<TJob>(Internal.JobParallelForBox<TJob> box)
        where TJob : struct, IJobFor
    {
        RunParallelFor(box.Begin, box.End, box.BatchSize, box.Job, box.Token);
    }

    /// <summary>Range-body variant: body receives [lo, hi) once per batch, reducing per-index dispatch overhead.</summary>
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
                GetDependencyTask(dependency),
                cancellationToken));
        }

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

    public static void RunFor<TJob>(TJob job, int length)
        where TJob : struct, IJobFor
        => Schedule(job, length).Complete();

    public static unsafe void Wait(JobHandle handle)
    {
        NativeJobState? nativeState = handle.NativeState;
        if (nativeState != null)
        {
            nativeState.Complete();
            return;
        }
        handle.InnerTask?.GetAwaiter().GetResult();
    }

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
            if (!handle.IsValid)
            {
                continue;
            }

            tasks ??= new List<Task>();
            tasks.Add(handle.AsTask());
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
            if (!handle.IsValid)
            {
                continue;
            }

            tasks ??= new List<Task>();
            tasks.Add(handle.AsTask());
        }

        return tasks == null || tasks.Count == 0
            ? default
            : ToHandle(Task.WhenAll(tasks));
    }

    public static void WaitAll(params JobHandle[] handles)
    {
        CombineDependencies(handles).Complete();
    }

    private static Task? GetDependencyTask(JobHandle dependency)
    {
        return dependency.IsValid ? dependency.AsTask() : null;
    }

    private static Task StartScheduled(Action run, Task? dependency, CancellationToken cancellationToken)
    {
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
