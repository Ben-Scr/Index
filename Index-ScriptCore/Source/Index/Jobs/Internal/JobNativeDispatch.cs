using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Index.Interop;

namespace Index.Jobs.Internal;

// Bridges managed Schedule paths into the native JobSystem bindings
// added in ScriptGlue.hpp. Three pieces:
//
//   1. IJobBox / IJobRangeBox — the interfaces every per-loop-shape
//      box (JobBox, JobForBox, JobParallelForBox, JobQueryRangeBox)
//      implements. The dispatch layer treats them uniformly.
//
//   2. [UnmanagedCallersOnly] entry points — the function pointers we
//      hand to native. They retrieve the box from the GCHandle we
//      passed as `context`, invoke it, and stash any exception via
//      JobExceptionRegistry for the calling thread to rethrow.
//
//   3. Schedule / ScheduleParallelFor helpers — pin the box behind a
//      GCHandle, hand it to native, return the ulong native handle
//      packaged as a JobHandle.
internal static unsafe class JobNativeDispatch
{
    internal interface IJobBox
    {
        ulong NativeHandle { get; set; }

        // Invoked exactly once on a native worker. May throw — the
        // entry point catches and stashes the exception.
        void Invoke();

        // Called from the native side's release callback, after Wait
        // completes (so after every batch's Invoke / InvokeRange has
        // returned). Box should return itself to its per-thread pool
        // and clear any disposable state it holds.
        void OnRelease();
    }

    internal interface IJobRangeBox
    {
        ulong NativeHandle { get; set; }

        // Per-batch entry point — may be invoked concurrently across
        // worker threads, once per partition. Implementations must
        // make their own thread-local copy of any TJob struct they
        // hold so concurrent partitions don't trample each other.
        void InvokeRange(int lo, int hi);

        void OnRelease();
    }

    // ── [UnmanagedCallersOnly] entry points ──────────────────────────

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

    // ── Schedule helpers ─────────────────────────────────────────────

    internal static JobHandle Schedule(IJobBox box)
    {
        ref NativeBindingsStruct b = ref NativeCallbacks.Bindings;
        if (b.JobSystem_Enqueue == null)
        {
            // Native bindings not yet wired (shouldn't happen at runtime,
            // but a defensive inline run keeps tests sane in mocks).
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
