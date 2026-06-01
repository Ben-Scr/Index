using System;
using System.Collections.Generic;
using System.Threading;

namespace Index.Jobs.Internal;

internal static class JobPoolConfig
{
    internal const int Cap = 4;
}

internal sealed class JobBox<TJob> : JobNativeDispatch.IJobBox where TJob : struct, IJob
{
    internal TJob Job;
    internal CancellationToken Token;
    internal readonly Action ExecuteAction;

    public ulong NativeHandle { get; set; }

    internal JobBox()
    {
        ExecuteAction = Run;
    }

    // Used when we still go through the .NET Task path (fallback when
    // native bindings aren't wired). The native dispatch path uses
    // Invoke() / OnRelease() below.
    private void Run()
    {
        try
        {
            Token.ThrowIfCancellationRequested();
            Job.Execute();
        }
        finally
        {
            JobBoxPool<TJob>.Return(this);
        }
    }

    public void Invoke()
    {
        Token.ThrowIfCancellationRequested();
        Job.Execute();
    }

    public void OnRelease()
    {
        JobBoxPool<TJob>.Return(this);
    }
}

internal static class JobBoxPool<TJob> where TJob : struct, IJob
{
    [ThreadStatic] private static Stack<JobBox<TJob>>? t_Pool;

    internal static JobBox<TJob> Rent()
    {
        Stack<JobBox<TJob>>? pool = t_Pool;
        if (pool != null && pool.Count > 0)
        {
            return pool.Pop();
        }
        return new JobBox<TJob>();
    }

    internal static void Return(JobBox<TJob> box)
    {
        Stack<JobBox<TJob>>? pool = t_Pool ??= new Stack<JobBox<TJob>>(JobPoolConfig.Cap);
        if (pool.Count < JobPoolConfig.Cap)
        {
            // Clear payload so the pool doesn't pin references between rents.
            box.Job = default;
            box.Token = default;
            pool.Push(box);
        }
    }
}

internal sealed class JobForBox<TJob> : JobNativeDispatch.IJobBox where TJob : struct, IJobFor
{
    internal TJob Job;
    internal CancellationToken Token;
    internal int Length;
    internal readonly Action ExecuteAction;

    public ulong NativeHandle { get; set; }

    internal JobForBox()
    {
        ExecuteAction = Run;
    }

    private void Run()
    {
        try
        {
            InvokeLoop();
        }
        finally
        {
            JobForBoxPool<TJob>.Return(this);
        }
    }

    public void Invoke()
    {
        InvokeLoop();
    }

    public void OnRelease()
    {
        JobForBoxPool<TJob>.Return(this);
    }

    private void InvokeLoop()
    {
        TJob local = Job;
        CancellationToken token = Token;
        int length = Length;
        for (int i = 0; i < length; ++i)
        {
            token.ThrowIfCancellationRequested();
            local.Execute(i);
        }
    }
}

internal static class JobForBoxPool<TJob> where TJob : struct, IJobFor
{
    [ThreadStatic] private static Stack<JobForBox<TJob>>? t_Pool;

    internal static JobForBox<TJob> Rent()
    {
        Stack<JobForBox<TJob>>? pool = t_Pool;
        if (pool != null && pool.Count > 0)
        {
            return pool.Pop();
        }
        return new JobForBox<TJob>();
    }

    internal static void Return(JobForBox<TJob> box)
    {
        Stack<JobForBox<TJob>>? pool = t_Pool ??= new Stack<JobForBox<TJob>>(JobPoolConfig.Cap);
        if (pool.Count < JobPoolConfig.Cap)
        {
            box.Job = default;
            box.Token = default;
            box.Length = 0;
            pool.Push(box);
        }
    }
}

internal sealed class JobParallelForBox<TJob> : JobNativeDispatch.IJobRangeBox where TJob : struct, IJobFor
{
    internal TJob Job;
    internal CancellationToken Token;
    internal int Begin;
    internal int End;
    internal int BatchSize;
    internal readonly Action ExecuteAction;

    public ulong NativeHandle { get; set; }

    internal JobParallelForBox()
    {
        ExecuteAction = Run;
    }

    private void Run()
    {
        try
        {
            global::Index.Jobs.Job.RunParallelForFromBox(this);
        }
        finally
        {
            JobParallelForBoxPool<TJob>.Return(this);
        }
    }

    public void InvokeRange(int lo, int hi)
    {
        // Copy the job struct PER BATCH — matches the existing
        // "Parallel.ForEach copies per partition" semantics that user
        // code already relies on (any shared mutable state must use
        // AtomicInt / ParallelReducer).
        TJob local = Job;
        CancellationToken token = Token;
        for (int i = lo; i < hi; ++i)
        {
            token.ThrowIfCancellationRequested();
            local.Execute(i);
        }
    }

    public void OnRelease()
    {
        JobParallelForBoxPool<TJob>.Return(this);
    }
}

internal static class JobParallelForBoxPool<TJob> where TJob : struct, IJobFor
{
    [ThreadStatic] private static Stack<JobParallelForBox<TJob>>? t_Pool;

    internal static JobParallelForBox<TJob> Rent()
    {
        Stack<JobParallelForBox<TJob>>? pool = t_Pool;
        if (pool != null && pool.Count > 0)
        {
            return pool.Pop();
        }
        return new JobParallelForBox<TJob>();
    }

    internal static void Return(JobParallelForBox<TJob> box)
    {
        Stack<JobParallelForBox<TJob>>? pool = t_Pool ??= new Stack<JobParallelForBox<TJob>>(JobPoolConfig.Cap);
        if (pool.Count < JobPoolConfig.Cap)
        {
            box.Job = default;
            box.Token = default;
            box.Begin = 0;
            box.End = 0;
            box.BatchSize = 0;
            pool.Push(box);
        }
    }
}

internal sealed class RangeBodyBox : JobNativeDispatch.IJobRangeBox
{
    internal Action<int, int>? Body;
    internal CancellationToken Token;

    public ulong NativeHandle { get; set; }

    [ThreadStatic] private static Stack<RangeBodyBox>? t_Pool;

    internal static RangeBodyBox Rent(Action<int, int> body, CancellationToken token)
    {
        Stack<RangeBodyBox>? pool = t_Pool;
        RangeBodyBox box = (pool != null && pool.Count > 0) ? pool.Pop() : new RangeBodyBox();
        box.Body = body;
        box.Token = token;
        return box;
    }

    public void InvokeRange(int lo, int hi)
    {
        Token.ThrowIfCancellationRequested();
        Action<int, int>? body = Body;
        if (body != null)
        {
            body(lo, hi);
        }
    }

    public void OnRelease()
    {
        Stack<RangeBodyBox>? pool = t_Pool ??= new Stack<RangeBodyBox>(JobPoolConfig.Cap);
        if (pool.Count < JobPoolConfig.Cap)
        {
            Body = null;
            Token = default;
            pool.Push(this);
        }
    }
}
