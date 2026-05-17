using System;
using System.Threading;

namespace Index.Jobs;

/// <summary>
/// Per-thread accumulator that merges into a single result on demand.
/// <para>
/// Prefer this over a hot <see cref="AtomicInt"/> / <see cref="AtomicFloat"/>
/// in the inner loop of an <c>IJobParallelFor</c>: every worker mutates its own
/// thread-local cell with no contention, and the final reduce happens once.
/// </para>
/// <example>
/// <code>
/// var sum = ParallelReducer.SumInt();
/// Job.ParallelFor(values.Length, i => sum.Add(values[i]));
/// int total = sum.Result();
/// </code>
/// </example>
/// </summary>
public sealed class ParallelReducer<T> : IDisposable
{
    private readonly Func<T, T, T> m_Combine;
    private readonly T m_Identity;
    private readonly ThreadLocal<StrongBox> m_Local;

    // Intrusive lock-free stack of every StrongBox ever produced. Each
    // box is linked in exactly once (in the ThreadLocal factory, which
    // runs at most once per worker thread). Result()/Reset() walk this
    // chain without allocating an enumerator — the previous
    // implementation hit ThreadLocal.Values which snapshots into a
    // fresh IList<T> on every call.
    private StrongBox? m_Head;

    public ParallelReducer(T identity, Func<T, T, T> combine)
    {
        ArgumentNullException.ThrowIfNull(combine);
        m_Identity = identity;
        m_Combine = combine;
        m_Local = new ThreadLocal<StrongBox>(CreateBox);
    }

    private StrongBox CreateBox()
    {
        var box = new StrongBox(m_Identity);
        // Push onto the head with CAS retry. Per-worker first-use cost
        // only — never on the hot Add() path.
        StrongBox? current;
        do
        {
            current = Volatile.Read(ref m_Head);
            box.Next = current;
        } while (Interlocked.CompareExchange(ref m_Head, box, current) != current);
        return box;
    }

    /// <summary>
    /// Folds <paramref name="value"/> into the calling thread's cell.
    /// Thread-safe by virtue of being thread-local; no atomics needed.
    /// </summary>
    public void Add(T value)
    {
        StrongBox box = m_Local.Value!;
        box.Value = m_Combine(box.Value, value);
    }

    /// <summary>
    /// Collapses all per-thread cells into one result using the combiner.
    /// Call this after the parallel job has completed.
    /// </summary>
    public T Result()
    {
        T acc = m_Identity;
        StrongBox? node = Volatile.Read(ref m_Head);
        while (node != null)
        {
            acc = m_Combine(acc, node.Value);
            node = node.Next;
        }
        return acc;
    }

    /// <summary>
    /// Resets every thread-local cell back to the identity value.
    /// </summary>
    public void Reset()
    {
        StrongBox? node = Volatile.Read(ref m_Head);
        while (node != null)
        {
            node.Value = m_Identity;
            node = node.Next;
        }
    }

    public void Dispose() => m_Local.Dispose();

    private sealed class StrongBox
    {
        public T Value;
        public StrongBox? Next;

        public StrongBox(T value)
        {
            Value = value;
        }
    }
}

/// <summary>
/// Convenience factories for the common reduction shapes.
/// </summary>
public static class ParallelReducer
{
    public static ParallelReducer<int> SumInt()
        => new ParallelReducer<int>(0, static (a, b) => a + b);

    public static ParallelReducer<long> SumLong()
        => new ParallelReducer<long>(0L, static (a, b) => a + b);

    public static ParallelReducer<float> SumFloat()
        => new ParallelReducer<float>(0f, static (a, b) => a + b);

    public static ParallelReducer<double> SumDouble()
        => new ParallelReducer<double>(0d, static (a, b) => a + b);

    public static ParallelReducer<int> MinInt()
        => new ParallelReducer<int>(int.MaxValue, static (a, b) => a < b ? a : b);

    public static ParallelReducer<int> MaxInt()
        => new ParallelReducer<int>(int.MinValue, static (a, b) => a > b ? a : b);

    public static ParallelReducer<float> MinFloat()
        => new ParallelReducer<float>(float.PositiveInfinity, static (a, b) => a < b ? a : b);

    public static ParallelReducer<float> MaxFloat()
        => new ParallelReducer<float>(float.NegativeInfinity, static (a, b) => a > b ? a : b);
}
