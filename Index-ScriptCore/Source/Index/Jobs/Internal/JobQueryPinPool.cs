using System;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.InteropServices;

namespace Index.Jobs.Internal;

// Pools pre-pinned IntPtr[] buffers for IJobQuery row-pointer snapshots. Without pooling, high-frequency jobs churn the LOH and block Gen2 compaction. Bucketed by power-of-2 size; oversized buffers bypass the pool entirely.
internal sealed class JobQueryPinPool
{
    private const int Cap = 8;
    private const int MaxBuckets = 24; // 2^23 = ~8M cells, comfortably above any realistic scene

    private static readonly Stack<PinnedBuffer>?[] s_Buckets = new Stack<PinnedBuffer>?[MaxBuckets];
    private static readonly object s_Lock = new();

    internal sealed class PinnedBuffer
    {
        internal readonly IntPtr[] Array;
        internal readonly GCHandle Handle;
        internal readonly IntPtr PinnedBase;
        internal readonly int Bucket; // -1 when this buffer bypasses the pool

        internal PinnedBuffer(int bucket, int capacity)
        {
            Array = new IntPtr[capacity];
            Handle = GCHandle.Alloc(Array, GCHandleType.Pinned);
            PinnedBase = Handle.AddrOfPinnedObject();
            Bucket = bucket;
        }
    }

    internal static PinnedBuffer Rent(int minSize)
    {
        int bucket = BucketIndex(minSize);
        if (bucket < MaxBuckets)
        {
            lock (s_Lock)
            {
                Stack<PinnedBuffer>? bag = s_Buckets[bucket];
                if (bag != null && bag.Count > 0)
                {
                    return bag.Pop();
                }
            }
            return new PinnedBuffer(bucket, 1 << bucket);
        }
        // Too big for the pool — allocate fresh, mark as not-poolable.
        return new PinnedBuffer(bucket: -1, capacity: minSize);
    }

    internal static void Return(PinnedBuffer buf)
    {
        if (buf.Bucket < 0 || buf.Bucket >= MaxBuckets)
        {
            // Buffer was never in the pool; free its pin and let the
            // array be reclaimed by GC.
            if (buf.Handle.IsAllocated) buf.Handle.Free();
            return;
        }

        lock (s_Lock)
        {
            Stack<PinnedBuffer>? bag = s_Buckets[buf.Bucket] ??= new Stack<PinnedBuffer>(Cap);
            if (bag.Count < Cap)
            {
                bag.Push(buf);
                return;
            }
        }
        // Bucket full — release this one.
        if (buf.Handle.IsAllocated) buf.Handle.Free();
    }

    private static int BucketIndex(int minSize)
    {
        if (minSize <= 1) return 0;
        // ceil(log2(minSize)) = 32 - lzcnt(minSize - 1) for minSize > 1.
        return 32 - BitOperations.LeadingZeroCount((uint)(minSize - 1));
    }
}
