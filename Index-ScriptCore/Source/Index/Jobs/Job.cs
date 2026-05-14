using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;

namespace Index.Jobs;

public interface IJob
{
    void Execute();
}

public interface IJobFor
{
    void Execute(int index);
}

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
    private readonly Task? m_Task;

    internal JobHandle(Task? task)
    {
        m_Task = task;
    }

    internal Task? InnerTask => m_Task;

    public bool IsValid => m_Task != null;
    public bool IsComplete => m_Task == null || m_Task.IsCompleted;
    public bool IsCompleted => IsComplete;
    public bool IsFaulted => m_Task?.IsFaulted ?? false;
    public bool IsCanceled => m_Task?.IsCanceled ?? false;
    public Exception? Exception => m_Task?.Exception?.GetBaseException();

    public void Complete() => Job.Wait(this);
    public Task AsTask() => m_Task ?? Task.CompletedTask;
    public TaskAwaiter GetAwaiter() => AsTask().GetAwaiter();
}

public static class JobSystem
{
    private static int s_WorkerCount = ResolveWorkerCount(-1);

    public static bool IsInitialized => true;
    public static int WorkerCount => Volatile.Read(ref s_WorkerCount);
    public static bool IsCallerWorker => Thread.CurrentThread.IsThreadPoolThread;
    public static int WorkerIndex => -1;

    public static void Initialize(JobSystemSpec spec = default)
    {
        Configure(spec.WorkerCount == 0 ? -1 : spec.WorkerCount);
    }

    public static void Configure(int workerCount = -1)
    {
        int resolved = ResolveWorkerCount(workerCount);
        Volatile.Write(ref s_WorkerCount, resolved);

        ThreadPool.GetMinThreads(out int minWorkerThreads, out int minCompletionPortThreads);
        if (minWorkerThreads < resolved)
        {
            ThreadPool.SetMinThreads(resolved, minCompletionPortThreads);
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
        => ToHandle(StartScheduled(() => job.Execute(), dependency.InnerTask, cancellationToken));

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
        if (length < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(length));
        }

        if (length == 0)
        {
            return default;
        }

        return ToHandle(StartScheduled(
            () => RunParallelFor(0, length, batchSize, job, cancellationToken),
            dependency.InnerTask,
            cancellationToken));
    }

    public static void ParallelFor(int length, Action<int> body, int batchSize = 0)
        => ScheduleParallelFor(length, body, batchSize).Complete();

    public static void ParallelFor(int begin, int end, Action<int> body, int batchSize = 0)
        => ScheduleParallelFor(begin, end, body, batchSize).Complete();

    public static void ParallelFor<TJob>(TJob job, int length, int batchSize = 0)
        where TJob : struct, IJobFor
        => ScheduleParallelFor(job, length, batchSize).Complete();

    public static void Wait(JobHandle handle)
    {
        handle.InnerTask?.GetAwaiter().GetResult();
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
        ObserveFaults(task);
        return new JobHandle(task);
    }

    private static void ObserveFaults(Task task)
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
        var options = new ParallelOptions {
            CancellationToken = cancellationToken,
            MaxDegreeOfParallelism = JobSystem.WorkerCount,
        };

        Parallel.ForEach(
            Partitioner.Create(begin, end, resolvedBatchSize),
            options,
            range =>
            {
                for (int i = range.Item1; i < range.Item2; ++i)
                {
                    options.CancellationToken.ThrowIfCancellationRequested();
                    body(i);
                }
            });
    }

    private static void RunParallelFor<TJob>(int begin, int end, int batchSize, TJob job, CancellationToken cancellationToken)
        where TJob : struct, IJobFor
    {
        int resolvedBatchSize = ResolveBatchSize(begin, end, batchSize);
        var options = new ParallelOptions {
            CancellationToken = cancellationToken,
            MaxDegreeOfParallelism = JobSystem.WorkerCount,
        };

        Parallel.ForEach(
            Partitioner.Create(begin, end, resolvedBatchSize),
            options,
            range =>
            {
                TJob localJob = job;
                for (int i = range.Item1; i < range.Item2; ++i)
                {
                    options.CancellationToken.ThrowIfCancellationRequested();
                    localJob.Execute(i);
                }
            });
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
