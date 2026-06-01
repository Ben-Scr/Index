using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Index.Interop;

namespace Index.Jobs.Internal;

internal static unsafe class JobNativeDispatch
{
    internal interface IJobBox
    {
        ulong NativeHandle { get; set; }

        void Invoke();
        void OnRelease();
    }

    internal interface IJobRangeBox
    {
        ulong NativeHandle { get; set; }

        // Invoked concurrently across worker threads; implementations MUST copy the TJob struct locally to avoid concurrent partition trampling.
        void InvokeRange(int lo, int hi);

        void OnRelease();
    }

    [UnmanagedCallersOnly]
    private static void JobEntrypoint(void* ctx)
    {
        GCHandle gch = GCHandle.FromIntPtr((IntPtr)ctx);
        if (!gch.IsAllocated) return;
        IJobBox? box = gch.Target as IJobBox;
        if (box == null) return;

        try
        {
            box.Invoke();
        }
        catch (Exception ex)
        {
            JobExceptionRegistry.Stash(box.NativeHandle, ex);
        }
    }

    [UnmanagedCallersOnly]
    private static void JobRangeEntrypoint(void* ctx, int lo, int hi)
    {
        GCHandle gch = GCHandle.FromIntPtr((IntPtr)ctx);
        if (!gch.IsAllocated) return;
        IJobRangeBox? box = gch.Target as IJobRangeBox;
        if (box == null) return;

        try
        {
            box.InvokeRange(lo, hi);
        }
        catch (Exception ex)
        {
            JobExceptionRegistry.Stash(box.NativeHandle, ex);
        }
    }

    // Called by native side on JobSystem_Release — after Wait has
    // completed and any consumer of the handle has had a chance to
    // observe the exception via Complete().
    [UnmanagedCallersOnly]
    private static void ReleaseContext(void* ctx)
    {
        GCHandle gch = GCHandle.FromIntPtr((IntPtr)ctx);
        if (!gch.IsAllocated) return;

        // Surface unconsumed exceptions before freeing the box — keeps
        // fire-and-forget paths from silently swallowing faults.
        ulong nh = 0;
        switch (gch.Target)
        {
            case IJobBox singleBox:
                nh = singleBox.NativeHandle;
                singleBox.OnRelease();
                break;
            case IJobRangeBox rangeBox:
                nh = rangeBox.NativeHandle;
                rangeBox.OnRelease();
                break;
        }
        JobExceptionRegistry.DrainOrLog(nh);
        gch.Free();
    }

    internal static JobHandle Schedule(IJobBox box)
    {
        ref NativeBindingsStruct b = ref NativeCallbacks.Bindings;
        if (b.JobSystem_Enqueue == null)
        {
            box.NativeHandle = 0;
            try { box.Invoke(); }
            finally { box.OnRelease(); }
            return default;
        }

        GCHandle gch = GCHandle.Alloc(box);
        ulong nh = b.JobSystem_Enqueue(
            &JobEntrypoint,
            (void*)GCHandle.ToIntPtr(gch),
            &ReleaseContext);
        box.NativeHandle = nh;
        return new JobHandle(nh);
    }

    internal static JobHandle ScheduleParallelFor(IJobRangeBox box, int begin, int end, int batchSize)
    {
        ref NativeBindingsStruct b = ref NativeCallbacks.Bindings;
        if (b.JobSystem_ParallelFor == null)
        {
            box.NativeHandle = 0;
            try { box.InvokeRange(begin, end); }
            finally { box.OnRelease(); }
            return default;
        }

        GCHandle gch = GCHandle.Alloc(box);
        ulong nh = b.JobSystem_ParallelFor(
            begin, end, batchSize,
            &JobRangeEntrypoint,
            (void*)GCHandle.ToIntPtr(gch),
            &ReleaseContext);
        box.NativeHandle = nh;
        return new JobHandle(nh);
    }
}
