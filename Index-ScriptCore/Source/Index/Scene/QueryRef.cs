using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using Index.Components;
using Index.Interop;

namespace Index;

// MUST NOT perform structural changes (Add/Remove/Destroy/Create) inside a QueryRef foreach body — EnTT pools may reallocate and invalidate the pointers. Use Scene.Query<T>() for mid-iteration mutation.
// Nesting QueryRef foreach loops on the same thread IS safe: each enumerator checks out its own buffer from a per-thread pool (QueryRefBuffers) and returns it on Dispose.

internal struct QueryRefFilters
{
    // Pipe-separated component display names — same format the legacy query
    // path uses. Mutable so the builder ref-struct can append per chain step.
    public string SceneName;
    public string WriteNames;
    public string ReadonlyNames;
    public string MustHaveNames;
    public string WithoutNames;
    public int    EnableFilter;   // 0=all, 1=enabled, 2=disabled

    public static QueryRefFilters Create(string sceneName)
    {
        return new QueryRefFilters {
            SceneName     = sceneName,
            WriteNames    = "",
            ReadonlyNames = "",
            MustHaveNames = "",
            WithoutNames  = "",
            EnableFilter  = 1,
        };
    }

    public void AppendWrite(string? name)
    {
        if (string.IsNullOrEmpty(name)) return;
        WriteNames = string.IsNullOrEmpty(WriteNames) ? name : WriteNames + "|" + name;
    }

    public void AppendWithout(string? name)
    {
        if (string.IsNullOrEmpty(name)) return;
        WithoutNames = string.IsNullOrEmpty(WithoutNames) ? name : WithoutNames + "|" + name;
    }

    public void AppendMustHave(string? name)
    {
        if (string.IsNullOrEmpty(name)) return;
        MustHaveNames = string.IsNullOrEmpty(MustHaveNames) ? name : MustHaveNames + "|" + name;
    }
}

internal static class QueryRefBuffers
{
    // Per-thread POOL of buffers (not a single shared array). Each active enumerator
    // checks one out in Open()/OpenWithEntities() and returns it in its Dispose()
    // (foreach disposes ref-struct enumerators). Nested QueryRef foreach loops on the
    // same thread therefore each own a DISTINCT buffer — previously they all aliased
    // one [ThreadStatic] array, so an inner loop's Open() overwrote the outer loop's
    // component pointers and Current returned refs into the wrong rows (silent native
    // memory corruption).
    [ThreadStatic] private static Stack<IntPtr[]>? s_Pool;
    [ThreadStatic] private static Stack<ulong[]>?  s_EntityPool;

    private static IntPtr[] CheckoutPtr(int minSize)
    {
        Stack<IntPtr[]> pool = s_Pool ??= new Stack<IntPtr[]>();
        IntPtr[] buf = pool.Count > 0 ? pool.Pop() : new IntPtr[Math.Max(minSize, 64)];
        if (buf.Length < minSize)
            buf = new IntPtr[minSize];
        return buf;
    }

    private static ulong[] CheckoutEntityIds(int minSize)
    {
        Stack<ulong[]> pool = s_EntityPool ??= new Stack<ulong[]>();
        ulong[] buf = pool.Count > 0 ? pool.Pop() : new ulong[Math.Max(minSize, 64)];
        if (buf.Length < minSize)
            buf = new ulong[minSize];
        return buf;
    }

    internal static void Return(IntPtr[] buffer)
    {
        (s_Pool ??= new Stack<IntPtr[]>()).Push(buffer);
    }

    internal static void ReturnWithEntities(IntPtr[] buffer, ulong[] entityIds)
    {
        (s_Pool ??= new Stack<IntPtr[]>()).Push(buffer);
        (s_EntityPool ??= new Stack<ulong[]>()).Push(entityIds);
    }

    // Two-call growth: open the view with the checked-out buffer; if the native
    // side reports more rows than fit, grow once and retry. The caller (enumerator)
    // owns the returned buffer until its Dispose() returns it to the pool.
    internal static unsafe (IntPtr[] buffer, int rowCount) Open(
        ref QueryRefFilters filters, int poolCount)
    {
        IntPtr[] buf = CheckoutPtr(64 * poolCount);
        while (true)
        {
            int rowCap = buf.Length / poolCount;
            int rowCount;
            fixed (IntPtr* p = buf)
            {
                rowCount = InternalCalls.Scene_OpenQueryView(
                    filters.SceneName, filters.WriteNames, filters.ReadonlyNames,
                    filters.MustHaveNames, filters.WithoutNames, filters.EnableFilter,
                    (void**)p, rowCap);
            }
            if (rowCount <= rowCap) return (buf, rowCount);
            // Grow and retry — rare path. The small checked-out buffer is dropped
            // (GC'd); the grown buffer is what Dispose() returns to the pool, so the
            // pool's checkout/return count stays balanced.
            buf = new IntPtr[rowCount * poolCount];
        }
    }

    internal static unsafe (IntPtr[] buffer, ulong[] entityIds, int rowCount) OpenWithEntities(
        ref QueryRefFilters filters, int poolCount)
    {
        IntPtr[] buf = CheckoutPtr(64 * poolCount);
        ulong[] entityIds = CheckoutEntityIds(64);
        while (true)
        {
            int rowCap = Math.Min(buf.Length / poolCount, entityIds.Length);
            int rowCount;
            fixed (IntPtr* p = buf)
            fixed (ulong* e = entityIds)
            {
                rowCount = InternalCalls.Scene_OpenQueryViewWithEntities(
                    filters.SceneName, filters.WriteNames, filters.ReadonlyNames,
                    filters.MustHaveNames, filters.WithoutNames, filters.EnableFilter,
                    (void**)p, e, rowCap);
            }
            if (rowCount <= rowCap) return (buf, entityIds, rowCount);
            buf = new IntPtr[rowCount * poolCount];
            entityIds = new ulong[rowCount];
        }
    }
}

// ── Single-component query ───────────────────────────────────────────

public ref struct QueryRefBuilder1<TW1> where TW1 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefBuilder1(string sceneName)
    {
        m_Filters = QueryRefFilters.Create(sceneName);
        m_Filters.AppendWrite(ComponentTypes<TW1>.NativeName);
    }

    public QueryRefBuilder1<TW1> Without<T>() where T : unmanaged, IComponent
    { m_Filters.AppendWithout(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder1<TW1> With<T>() where T : unmanaged, IComponent
    { m_Filters.AppendMustHave(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder1<TW1> EnabledOnly()  { m_Filters.EnableFilter = 1; return this; }
    public QueryRefBuilder1<TW1> IncludeDisabled() { m_Filters.EnableFilter = 0; return this; }
    public QueryRefBuilder1<TW1> DisabledOnly() { m_Filters.EnableFilter = 2; return this; }

    public QueryRefBuilder1_RO1<TW1, TRO1> Readonly<TRO1>() where TRO1 : unmanaged, IComponent
        => new QueryRefBuilder1_RO1<TW1, TRO1>(m_Filters);

    internal QueryRefFilters GetFilters() => m_Filters;

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_Count) = QueryRefBuffers.Open(ref filters, poolCount: 1);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        // foreach disposes ref-struct enumerators — return our buffer to the
        // per-thread pool so a sibling/outer query can reuse it without aliasing.
        public void Dispose() => QueryRefBuffers.Return(m_Buffer);

        public unsafe ref TW1 Current
            => ref Unsafe.AsRef<TW1>((void*)m_Buffer[m_Index]);
    }
}

// ── 1 write + 1 readonly ─────────────────────────────────────────────

public ref struct QueryRefBuilder1_RO1<TW1, TRO1>
    where TW1  : unmanaged, IComponent
    where TRO1 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefBuilder1_RO1(QueryRefFilters inherited)
    {
        m_Filters = inherited;
        m_Filters.ReadonlyNames = ComponentTypes<TRO1>.NativeName;
    }

    public QueryRefBuilder1_RO1<TW1, TRO1> Without<T>() where T : unmanaged, IComponent
    { m_Filters.AppendWithout(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder1_RO1<TW1, TRO1> With<T>() where T : unmanaged, IComponent
    { m_Filters.AppendMustHave(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder1_RO1<TW1, TRO1> EnabledOnly()  { m_Filters.EnableFilter = 1; return this; }
    public QueryRefBuilder1_RO1<TW1, TRO1> IncludeDisabled() { m_Filters.EnableFilter = 0; return this; }
    public QueryRefBuilder1_RO1<TW1, TRO1> DisabledOnly() { m_Filters.EnableFilter = 2; return this; }

    internal QueryRefFilters GetFilters() => m_Filters;

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_Count) = QueryRefBuffers.Open(ref filters, poolCount: 2);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        // foreach disposes ref-struct enumerators — return our buffer to the
        // per-thread pool so a sibling/outer query can reuse it without aliasing.
        public void Dispose() => QueryRefBuffers.Return(m_Buffer);

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 2;
                return new Row(
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TRO1*)m_Buffer[baseIdx + 1]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly unsafe TW1*  m_W1;
        private readonly unsafe TRO1* m_RO1;

        internal unsafe Row(TW1* w1, TRO1* ro1) { m_W1 = w1; m_RO1 = ro1; }

        public unsafe ref TW1          W => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref readonly TRO1 R => ref Unsafe.AsRef<TRO1>(m_RO1);

        public unsafe void Deconstruct(out Ref<TW1> w1, out RefRO<TRO1> ro1)
        {
            w1  = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            ro1 = new RefRO<TRO1>(in Unsafe.AsRef<TRO1>(m_RO1));
        }
    }
}

// 2-component writable query
public ref struct QueryRefBuilder2<TW1, TW2>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefBuilder2(string sceneName)
    {
        m_Filters = QueryRefFilters.Create(sceneName);
        m_Filters.AppendWrite(ComponentTypes<TW1>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW2>.NativeName);
    }

    public QueryRefBuilder2<TW1, TW2> Without<T>() where T : unmanaged, IComponent
    { m_Filters.AppendWithout(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder2<TW1, TW2> With<T>() where T : unmanaged, IComponent
    { m_Filters.AppendMustHave(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder2<TW1, TW2> EnabledOnly()  { m_Filters.EnableFilter = 1; return this; }
    public QueryRefBuilder2<TW1, TW2> IncludeDisabled() { m_Filters.EnableFilter = 0; return this; }
    public QueryRefBuilder2<TW1, TW2> DisabledOnly() { m_Filters.EnableFilter = 2; return this; }

    internal QueryRefFilters GetFilters() => m_Filters;

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_Count) = QueryRefBuffers.Open(ref filters, poolCount: 2);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        // foreach disposes ref-struct enumerators — return our buffer to the
        // per-thread pool so a sibling/outer query can reuse it without aliasing.
        public void Dispose() => QueryRefBuffers.Return(m_Buffer);

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 2;
                return new Row(
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TW2*)m_Buffer[baseIdx + 1]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;

        internal unsafe Row(TW1* w1, TW2* w2)
        {
            m_W1 = w1;
            m_W2 = w2;
        }

        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);

        public unsafe void Deconstruct(out Ref<TW1> w1, out Ref<TW2> w2)
        {
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
        }
    }
}

// 3-component writable query
public ref struct QueryRefBuilder3<TW1, TW2, TW3>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
    where TW3 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefBuilder3(string sceneName)
    {
        m_Filters = QueryRefFilters.Create(sceneName);
        m_Filters.AppendWrite(ComponentTypes<TW1>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW2>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW3>.NativeName);
    }

    public QueryRefBuilder3<TW1, TW2, TW3> Without<T>() where T : unmanaged, IComponent
    { m_Filters.AppendWithout(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder3<TW1, TW2, TW3> With<T>() where T : unmanaged, IComponent
    { m_Filters.AppendMustHave(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder3<TW1, TW2, TW3> EnabledOnly()  { m_Filters.EnableFilter = 1; return this; }
    public QueryRefBuilder3<TW1, TW2, TW3> IncludeDisabled() { m_Filters.EnableFilter = 0; return this; }
    public QueryRefBuilder3<TW1, TW2, TW3> DisabledOnly() { m_Filters.EnableFilter = 2; return this; }

    internal QueryRefFilters GetFilters() => m_Filters;

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_Count) = QueryRefBuffers.Open(ref filters, poolCount: 3);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        // foreach disposes ref-struct enumerators — return our buffer to the
        // per-thread pool so a sibling/outer query can reuse it without aliasing.
        public void Dispose() => QueryRefBuffers.Return(m_Buffer);

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 3;
                return new Row(
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TW2*)m_Buffer[baseIdx + 1],
                    (TW3*)m_Buffer[baseIdx + 2]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;

        internal unsafe Row(TW1* w1, TW2* w2, TW3* w3)
        {
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
        }

        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);

        public unsafe void Deconstruct(out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3)
        {
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
            w3 = new Ref<TW3>(ref Unsafe.AsRef<TW3>(m_W3));
        }
    }
}

// 4-component writable query
public ref struct QueryRefBuilder4<TW1, TW2, TW3, TW4>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
    where TW3 : unmanaged, IComponent
    where TW4 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefBuilder4(string sceneName)
    {
        m_Filters = QueryRefFilters.Create(sceneName);
        m_Filters.AppendWrite(ComponentTypes<TW1>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW2>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW3>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW4>.NativeName);
    }

    public QueryRefBuilder4<TW1, TW2, TW3, TW4> Without<T>() where T : unmanaged, IComponent
    { m_Filters.AppendWithout(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder4<TW1, TW2, TW3, TW4> With<T>() where T : unmanaged, IComponent
    { m_Filters.AppendMustHave(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder4<TW1, TW2, TW3, TW4> EnabledOnly()  { m_Filters.EnableFilter = 1; return this; }
    public QueryRefBuilder4<TW1, TW2, TW3, TW4> IncludeDisabled() { m_Filters.EnableFilter = 0; return this; }
    public QueryRefBuilder4<TW1, TW2, TW3, TW4> DisabledOnly() { m_Filters.EnableFilter = 2; return this; }

    internal QueryRefFilters GetFilters() => m_Filters;

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_Count) = QueryRefBuffers.Open(ref filters, poolCount: 4);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        // foreach disposes ref-struct enumerators — return our buffer to the
        // per-thread pool so a sibling/outer query can reuse it without aliasing.
        public void Dispose() => QueryRefBuffers.Return(m_Buffer);

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 4;
                return new Row(
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TW2*)m_Buffer[baseIdx + 1],
                    (TW3*)m_Buffer[baseIdx + 2],
                    (TW4*)m_Buffer[baseIdx + 3]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;
        private readonly unsafe TW4* m_W4;

        internal unsafe Row(TW1* w1, TW2* w2, TW3* w3, TW4* w4)
        {
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
            m_W4 = w4;
        }

        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);
        public unsafe ref TW4 W4 => ref Unsafe.AsRef<TW4>(m_W4);

        public unsafe void Deconstruct(out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3, out Ref<TW4> w4)
        {
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
            w3 = new Ref<TW3>(ref Unsafe.AsRef<TW3>(m_W3));
            w4 = new Ref<TW4>(ref Unsafe.AsRef<TW4>(m_W4));
        }
    }
}

// 5-component writable query
public ref struct QueryRefBuilder5<TW1, TW2, TW3, TW4, TW5>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
    where TW3 : unmanaged, IComponent
    where TW4 : unmanaged, IComponent
    where TW5 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefBuilder5(string sceneName)
    {
        m_Filters = QueryRefFilters.Create(sceneName);
        m_Filters.AppendWrite(ComponentTypes<TW1>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW2>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW3>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW4>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW5>.NativeName);
    }

    public QueryRefBuilder5<TW1, TW2, TW3, TW4, TW5> Without<T>() where T : unmanaged, IComponent
    { m_Filters.AppendWithout(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder5<TW1, TW2, TW3, TW4, TW5> With<T>() where T : unmanaged, IComponent
    { m_Filters.AppendMustHave(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder5<TW1, TW2, TW3, TW4, TW5> EnabledOnly()  { m_Filters.EnableFilter = 1; return this; }
    public QueryRefBuilder5<TW1, TW2, TW3, TW4, TW5> IncludeDisabled() { m_Filters.EnableFilter = 0; return this; }
    public QueryRefBuilder5<TW1, TW2, TW3, TW4, TW5> DisabledOnly() { m_Filters.EnableFilter = 2; return this; }

    internal QueryRefFilters GetFilters() => m_Filters;

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_Count) = QueryRefBuffers.Open(ref filters, poolCount: 5);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        // foreach disposes ref-struct enumerators — return our buffer to the
        // per-thread pool so a sibling/outer query can reuse it without aliasing.
        public void Dispose() => QueryRefBuffers.Return(m_Buffer);

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 5;
                return new Row(
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TW2*)m_Buffer[baseIdx + 1],
                    (TW3*)m_Buffer[baseIdx + 2],
                    (TW4*)m_Buffer[baseIdx + 3],
                    (TW5*)m_Buffer[baseIdx + 4]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;
        private readonly unsafe TW4* m_W4;
        private readonly unsafe TW5* m_W5;

        internal unsafe Row(TW1* w1, TW2* w2, TW3* w3, TW4* w4, TW5* w5)
        {
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
            m_W4 = w4;
            m_W5 = w5;
        }

        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);
        public unsafe ref TW4 W4 => ref Unsafe.AsRef<TW4>(m_W4);
        public unsafe ref TW5 W5 => ref Unsafe.AsRef<TW5>(m_W5);

        public unsafe void Deconstruct(out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3, out Ref<TW4> w4, out Ref<TW5> w5)
        {
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
            w3 = new Ref<TW3>(ref Unsafe.AsRef<TW3>(m_W3));
            w4 = new Ref<TW4>(ref Unsafe.AsRef<TW4>(m_W4));
            w5 = new Ref<TW5>(ref Unsafe.AsRef<TW5>(m_W5));
        }
    }
}

// 6-component writable query
public ref struct QueryRefBuilder6<TW1, TW2, TW3, TW4, TW5, TW6>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
    where TW3 : unmanaged, IComponent
    where TW4 : unmanaged, IComponent
    where TW5 : unmanaged, IComponent
    where TW6 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefBuilder6(string sceneName)
    {
        m_Filters = QueryRefFilters.Create(sceneName);
        m_Filters.AppendWrite(ComponentTypes<TW1>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW2>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW3>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW4>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW5>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW6>.NativeName);
    }

    public QueryRefBuilder6<TW1, TW2, TW3, TW4, TW5, TW6> Without<T>() where T : unmanaged, IComponent
    { m_Filters.AppendWithout(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder6<TW1, TW2, TW3, TW4, TW5, TW6> With<T>() where T : unmanaged, IComponent
    { m_Filters.AppendMustHave(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder6<TW1, TW2, TW3, TW4, TW5, TW6> EnabledOnly()  { m_Filters.EnableFilter = 1; return this; }
    public QueryRefBuilder6<TW1, TW2, TW3, TW4, TW5, TW6> IncludeDisabled() { m_Filters.EnableFilter = 0; return this; }
    public QueryRefBuilder6<TW1, TW2, TW3, TW4, TW5, TW6> DisabledOnly() { m_Filters.EnableFilter = 2; return this; }

    internal QueryRefFilters GetFilters() => m_Filters;

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_Count) = QueryRefBuffers.Open(ref filters, poolCount: 6);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        // foreach disposes ref-struct enumerators — return our buffer to the
        // per-thread pool so a sibling/outer query can reuse it without aliasing.
        public void Dispose() => QueryRefBuffers.Return(m_Buffer);

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 6;
                return new Row(
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TW2*)m_Buffer[baseIdx + 1],
                    (TW3*)m_Buffer[baseIdx + 2],
                    (TW4*)m_Buffer[baseIdx + 3],
                    (TW5*)m_Buffer[baseIdx + 4],
                    (TW6*)m_Buffer[baseIdx + 5]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;
        private readonly unsafe TW4* m_W4;
        private readonly unsafe TW5* m_W5;
        private readonly unsafe TW6* m_W6;

        internal unsafe Row(TW1* w1, TW2* w2, TW3* w3, TW4* w4, TW5* w5, TW6* w6)
        {
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
            m_W4 = w4;
            m_W5 = w5;
            m_W6 = w6;
        }

        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);
        public unsafe ref TW4 W4 => ref Unsafe.AsRef<TW4>(m_W4);
        public unsafe ref TW5 W5 => ref Unsafe.AsRef<TW5>(m_W5);
        public unsafe ref TW6 W6 => ref Unsafe.AsRef<TW6>(m_W6);

        public unsafe void Deconstruct(out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3, out Ref<TW4> w4, out Ref<TW5> w5, out Ref<TW6> w6)
        {
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
            w3 = new Ref<TW3>(ref Unsafe.AsRef<TW3>(m_W3));
            w4 = new Ref<TW4>(ref Unsafe.AsRef<TW4>(m_W4));
            w5 = new Ref<TW5>(ref Unsafe.AsRef<TW5>(m_W5));
            w6 = new Ref<TW6>(ref Unsafe.AsRef<TW6>(m_W6));
        }
    }
}

// 7-component writable query
public ref struct QueryRefBuilder7<TW1, TW2, TW3, TW4, TW5, TW6, TW7>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
    where TW3 : unmanaged, IComponent
    where TW4 : unmanaged, IComponent
    where TW5 : unmanaged, IComponent
    where TW6 : unmanaged, IComponent
    where TW7 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefBuilder7(string sceneName)
    {
        m_Filters = QueryRefFilters.Create(sceneName);
        m_Filters.AppendWrite(ComponentTypes<TW1>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW2>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW3>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW4>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW5>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW6>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW7>.NativeName);
    }

    public QueryRefBuilder7<TW1, TW2, TW3, TW4, TW5, TW6, TW7> Without<T>() where T : unmanaged, IComponent
    { m_Filters.AppendWithout(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder7<TW1, TW2, TW3, TW4, TW5, TW6, TW7> With<T>() where T : unmanaged, IComponent
    { m_Filters.AppendMustHave(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder7<TW1, TW2, TW3, TW4, TW5, TW6, TW7> EnabledOnly()  { m_Filters.EnableFilter = 1; return this; }
    public QueryRefBuilder7<TW1, TW2, TW3, TW4, TW5, TW6, TW7> IncludeDisabled() { m_Filters.EnableFilter = 0; return this; }
    public QueryRefBuilder7<TW1, TW2, TW3, TW4, TW5, TW6, TW7> DisabledOnly() { m_Filters.EnableFilter = 2; return this; }

    internal QueryRefFilters GetFilters() => m_Filters;

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_Count) = QueryRefBuffers.Open(ref filters, poolCount: 7);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        // foreach disposes ref-struct enumerators — return our buffer to the
        // per-thread pool so a sibling/outer query can reuse it without aliasing.
        public void Dispose() => QueryRefBuffers.Return(m_Buffer);

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 7;
                return new Row(
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TW2*)m_Buffer[baseIdx + 1],
                    (TW3*)m_Buffer[baseIdx + 2],
                    (TW4*)m_Buffer[baseIdx + 3],
                    (TW5*)m_Buffer[baseIdx + 4],
                    (TW6*)m_Buffer[baseIdx + 5],
                    (TW7*)m_Buffer[baseIdx + 6]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;
        private readonly unsafe TW4* m_W4;
        private readonly unsafe TW5* m_W5;
        private readonly unsafe TW6* m_W6;
        private readonly unsafe TW7* m_W7;

        internal unsafe Row(TW1* w1, TW2* w2, TW3* w3, TW4* w4, TW5* w5, TW6* w6, TW7* w7)
        {
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
            m_W4 = w4;
            m_W5 = w5;
            m_W6 = w6;
            m_W7 = w7;
        }

        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);
        public unsafe ref TW4 W4 => ref Unsafe.AsRef<TW4>(m_W4);
        public unsafe ref TW5 W5 => ref Unsafe.AsRef<TW5>(m_W5);
        public unsafe ref TW6 W6 => ref Unsafe.AsRef<TW6>(m_W6);
        public unsafe ref TW7 W7 => ref Unsafe.AsRef<TW7>(m_W7);

        public unsafe void Deconstruct(out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3, out Ref<TW4> w4, out Ref<TW5> w5, out Ref<TW6> w6, out Ref<TW7> w7)
        {
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
            w3 = new Ref<TW3>(ref Unsafe.AsRef<TW3>(m_W3));
            w4 = new Ref<TW4>(ref Unsafe.AsRef<TW4>(m_W4));
            w5 = new Ref<TW5>(ref Unsafe.AsRef<TW5>(m_W5));
            w6 = new Ref<TW6>(ref Unsafe.AsRef<TW6>(m_W6));
            w7 = new Ref<TW7>(ref Unsafe.AsRef<TW7>(m_W7));
        }
    }
}

// 8-component writable query
public ref struct QueryRefBuilder8<TW1, TW2, TW3, TW4, TW5, TW6, TW7, TW8>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
    where TW3 : unmanaged, IComponent
    where TW4 : unmanaged, IComponent
    where TW5 : unmanaged, IComponent
    where TW6 : unmanaged, IComponent
    where TW7 : unmanaged, IComponent
    where TW8 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefBuilder8(string sceneName)
    {
        m_Filters = QueryRefFilters.Create(sceneName);
        m_Filters.AppendWrite(ComponentTypes<TW1>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW2>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW3>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW4>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW5>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW6>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW7>.NativeName);
        m_Filters.AppendWrite(ComponentTypes<TW8>.NativeName);
    }

    public QueryRefBuilder8<TW1, TW2, TW3, TW4, TW5, TW6, TW7, TW8> Without<T>() where T : unmanaged, IComponent
    { m_Filters.AppendWithout(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder8<TW1, TW2, TW3, TW4, TW5, TW6, TW7, TW8> With<T>() where T : unmanaged, IComponent
    { m_Filters.AppendMustHave(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder8<TW1, TW2, TW3, TW4, TW5, TW6, TW7, TW8> EnabledOnly()  { m_Filters.EnableFilter = 1; return this; }
    public QueryRefBuilder8<TW1, TW2, TW3, TW4, TW5, TW6, TW7, TW8> IncludeDisabled() { m_Filters.EnableFilter = 0; return this; }
    public QueryRefBuilder8<TW1, TW2, TW3, TW4, TW5, TW6, TW7, TW8> DisabledOnly() { m_Filters.EnableFilter = 2; return this; }

    internal QueryRefFilters GetFilters() => m_Filters;

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_Count) = QueryRefBuffers.Open(ref filters, poolCount: 8);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        // foreach disposes ref-struct enumerators — return our buffer to the
        // per-thread pool so a sibling/outer query can reuse it without aliasing.
        public void Dispose() => QueryRefBuffers.Return(m_Buffer);

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 8;
                return new Row(
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TW2*)m_Buffer[baseIdx + 1],
                    (TW3*)m_Buffer[baseIdx + 2],
                    (TW4*)m_Buffer[baseIdx + 3],
                    (TW5*)m_Buffer[baseIdx + 4],
                    (TW6*)m_Buffer[baseIdx + 5],
                    (TW7*)m_Buffer[baseIdx + 6],
                    (TW8*)m_Buffer[baseIdx + 7]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;
        private readonly unsafe TW4* m_W4;
        private readonly unsafe TW5* m_W5;
        private readonly unsafe TW6* m_W6;
        private readonly unsafe TW7* m_W7;
        private readonly unsafe TW8* m_W8;

        internal unsafe Row(TW1* w1, TW2* w2, TW3* w3, TW4* w4, TW5* w5, TW6* w6, TW7* w7, TW8* w8)
        {
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
            m_W4 = w4;
            m_W5 = w5;
            m_W6 = w6;
            m_W7 = w7;
            m_W8 = w8;
        }

        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);
        public unsafe ref TW4 W4 => ref Unsafe.AsRef<TW4>(m_W4);
        public unsafe ref TW5 W5 => ref Unsafe.AsRef<TW5>(m_W5);
        public unsafe ref TW6 W6 => ref Unsafe.AsRef<TW6>(m_W6);
        public unsafe ref TW7 W7 => ref Unsafe.AsRef<TW7>(m_W7);
        public unsafe ref TW8 W8 => ref Unsafe.AsRef<TW8>(m_W8);

        public unsafe void Deconstruct(out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3, out Ref<TW4> w4, out Ref<TW5> w5, out Ref<TW6> w6, out Ref<TW7> w7, out Ref<TW8> w8)
        {
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
            w3 = new Ref<TW3>(ref Unsafe.AsRef<TW3>(m_W3));
            w4 = new Ref<TW4>(ref Unsafe.AsRef<TW4>(m_W4));
            w5 = new Ref<TW5>(ref Unsafe.AsRef<TW5>(m_W5));
            w6 = new Ref<TW6>(ref Unsafe.AsRef<TW6>(m_W6));
            w7 = new Ref<TW7>(ref Unsafe.AsRef<TW7>(m_W7));
            w8 = new Ref<TW8>(ref Unsafe.AsRef<TW8>(m_W8));
        }
    }
}

// Extension entry point: `scene.QueryRef<T>()`. Lives outside the Scene class
// so the legacy SceneQuery.cs file stays untouched.
public static class SceneQueryRefExtensions
{
    public static QueryRefBuilder1<T> QueryRef<T>(this Scene scene)
        where T : unmanaged, IComponent
        => new QueryRefBuilder1<T>(scene.Name);

    public static QueryRefBuilder2<TW1, T2> QueryRef<TW1, T2>(this Scene scene)
        where TW1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        => new QueryRefBuilder2<TW1, T2>(scene.Name);

    public static QueryRefBuilder3<TW1, T2, T3> QueryRef<TW1, T2, T3>(this Scene scene)
        where TW1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        => new QueryRefBuilder3<TW1, T2, T3>(scene.Name);

    public static QueryRefBuilder4<TW1, T2, T3, T4> QueryRef<TW1, T2, T3, T4>(this Scene scene)
        where TW1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        => new QueryRefBuilder4<TW1, T2, T3, T4>(scene.Name);

    public static QueryRefBuilder5<TW1, T2, T3, T4, T5> QueryRef<TW1, T2, T3, T4, T5>(this Scene scene)
        where TW1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        => new QueryRefBuilder5<TW1, T2, T3, T4, T5>(scene.Name);

    public static QueryRefBuilder6<TW1, T2, T3, T4, T5, T6> QueryRef<TW1, T2, T3, T4, T5, T6>(this Scene scene)
        where TW1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        => new QueryRefBuilder6<TW1, T2, T3, T4, T5, T6>(scene.Name);

    public static QueryRefBuilder7<TW1, T2, T3, T4, T5, T6, T7> QueryRef<TW1, T2, T3, T4, T5, T6, T7>(this Scene scene)
        where TW1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
        => new QueryRefBuilder7<TW1, T2, T3, T4, T5, T6, T7>(scene.Name);

    public static QueryRefBuilder8<TW1, T2, T3, T4, T5, T6, T7, T8> QueryRef<TW1, T2, T3, T4, T5, T6, T7, T8>(this Scene scene)
        where TW1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
        where T8 : unmanaged, IComponent
        => new QueryRefBuilder8<TW1, T2, T3, T4, T5, T6, T7, T8>(scene.Name);
}
