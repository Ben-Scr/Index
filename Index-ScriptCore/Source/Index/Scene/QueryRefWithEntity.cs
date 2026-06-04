using System;
using System.Runtime.CompilerServices;
using Index.Components;

namespace Index;

public static class QueryRefWithEntityExtensions
{
    public static QueryRefEntityBuilder1<TW1> WithEntity<TW1>(this QueryRefBuilder1<TW1> builder)
        where TW1 : unmanaged, IComponent
        => new QueryRefEntityBuilder1<TW1>(builder.GetFilters());

    public static QueryRefEntityBuilder1_RO1<TW1, TRO1> WithEntity<TW1, TRO1>(this QueryRefBuilder1_RO1<TW1, TRO1> builder)
        where TW1 : unmanaged, IComponent
        where TRO1 : unmanaged, IComponent
        => new QueryRefEntityBuilder1_RO1<TW1, TRO1>(builder.GetFilters());

    public static QueryRefEntityBuilder2<TW1, TW2> WithEntity<TW1, TW2>(this QueryRefBuilder2<TW1, TW2> builder)
        where TW1 : unmanaged, IComponent
        where TW2 : unmanaged, IComponent
        => new QueryRefEntityBuilder2<TW1, TW2>(builder.GetFilters());

    public static QueryRefEntityBuilder3<TW1, TW2, TW3> WithEntity<TW1, TW2, TW3>(this QueryRefBuilder3<TW1, TW2, TW3> builder)
        where TW1 : unmanaged, IComponent
        where TW2 : unmanaged, IComponent
        where TW3 : unmanaged, IComponent
        => new QueryRefEntityBuilder3<TW1, TW2, TW3>(builder.GetFilters());

    public static QueryRefEntityBuilder4<TW1, TW2, TW3, TW4> WithEntity<TW1, TW2, TW3, TW4>(this QueryRefBuilder4<TW1, TW2, TW3, TW4> builder)
        where TW1 : unmanaged, IComponent
        where TW2 : unmanaged, IComponent
        where TW3 : unmanaged, IComponent
        where TW4 : unmanaged, IComponent
        => new QueryRefEntityBuilder4<TW1, TW2, TW3, TW4>(builder.GetFilters());

    public static QueryRefEntityBuilder5<TW1, TW2, TW3, TW4, TW5> WithEntity<TW1, TW2, TW3, TW4, TW5>(this QueryRefBuilder5<TW1, TW2, TW3, TW4, TW5> builder)
        where TW1 : unmanaged, IComponent
        where TW2 : unmanaged, IComponent
        where TW3 : unmanaged, IComponent
        where TW4 : unmanaged, IComponent
        where TW5 : unmanaged, IComponent
        => new QueryRefEntityBuilder5<TW1, TW2, TW3, TW4, TW5>(builder.GetFilters());

    public static QueryRefEntityBuilder6<TW1, TW2, TW3, TW4, TW5, TW6> WithEntity<TW1, TW2, TW3, TW4, TW5, TW6>(this QueryRefBuilder6<TW1, TW2, TW3, TW4, TW5, TW6> builder)
        where TW1 : unmanaged, IComponent
        where TW2 : unmanaged, IComponent
        where TW3 : unmanaged, IComponent
        where TW4 : unmanaged, IComponent
        where TW5 : unmanaged, IComponent
        where TW6 : unmanaged, IComponent
        => new QueryRefEntityBuilder6<TW1, TW2, TW3, TW4, TW5, TW6>(builder.GetFilters());

    public static QueryRefEntityBuilder7<TW1, TW2, TW3, TW4, TW5, TW6, TW7> WithEntity<TW1, TW2, TW3, TW4, TW5, TW6, TW7>(this QueryRefBuilder7<TW1, TW2, TW3, TW4, TW5, TW6, TW7> builder)
        where TW1 : unmanaged, IComponent
        where TW2 : unmanaged, IComponent
        where TW3 : unmanaged, IComponent
        where TW4 : unmanaged, IComponent
        where TW5 : unmanaged, IComponent
        where TW6 : unmanaged, IComponent
        where TW7 : unmanaged, IComponent
        => new QueryRefEntityBuilder7<TW1, TW2, TW3, TW4, TW5, TW6, TW7>(builder.GetFilters());

    public static QueryRefEntityBuilder8<TW1, TW2, TW3, TW4, TW5, TW6, TW7, TW8> WithEntity<TW1, TW2, TW3, TW4, TW5, TW6, TW7, TW8>(this QueryRefBuilder8<TW1, TW2, TW3, TW4, TW5, TW6, TW7, TW8> builder)
        where TW1 : unmanaged, IComponent
        where TW2 : unmanaged, IComponent
        where TW3 : unmanaged, IComponent
        where TW4 : unmanaged, IComponent
        where TW5 : unmanaged, IComponent
        where TW6 : unmanaged, IComponent
        where TW7 : unmanaged, IComponent
        where TW8 : unmanaged, IComponent
        => new QueryRefEntityBuilder8<TW1, TW2, TW3, TW4, TW5, TW6, TW7, TW8>(builder.GetFilters());
}

public ref struct QueryRefEntityBuilder1<TW1>
    where TW1 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefEntityBuilder1(QueryRefFilters filters)
    {
        m_Filters = filters;
    }

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly ulong[]  m_EntityIds;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_EntityIds, m_Count) = QueryRefBuffers.OpenWithEntities(ref filters, poolCount: 1);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 1;
                return new Row(
                    new EntityRef(m_EntityIds[m_Index]),
                    (TW1*)m_Buffer[baseIdx + 0]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly EntityRef m_Entity;
        private readonly unsafe TW1* m_W1;

        internal unsafe Row(EntityRef entity, TW1* w1)
        {
            m_Entity = entity;
            m_W1 = w1;
        }

        public EntityRef Entity => m_Entity;
        public unsafe ref TW1 W => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);

        public unsafe void Deconstruct(out EntityRef entity, out Ref<TW1> w1)
        {
            entity = m_Entity;
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
        }
    }
}

public ref struct QueryRefEntityBuilder1_RO1<TW1, TRO1>
    where TW1  : unmanaged, IComponent
    where TRO1 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefEntityBuilder1_RO1(QueryRefFilters filters)
    {
        m_Filters = filters;
    }

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly ulong[]  m_EntityIds;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_EntityIds, m_Count) = QueryRefBuffers.OpenWithEntities(ref filters, poolCount: 2);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 2;
                return new Row(
                    new EntityRef(m_EntityIds[m_Index]),
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TRO1*)m_Buffer[baseIdx + 1]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly EntityRef m_Entity;
        private readonly unsafe TW1*  m_W1;
        private readonly unsafe TRO1* m_RO1;

        internal unsafe Row(EntityRef entity, TW1* w1, TRO1* ro1)
        {
            m_Entity = entity;
            m_W1 = w1;
            m_RO1 = ro1;
        }

        public EntityRef Entity => m_Entity;
        public unsafe ref TW1 W => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref readonly TRO1 R => ref Unsafe.AsRef<TRO1>(m_RO1);
        public unsafe ref readonly TRO1 R1 => ref Unsafe.AsRef<TRO1>(m_RO1);

        public unsafe void Deconstruct(out EntityRef entity, out Ref<TW1> w1, out RefRO<TRO1> ro1)
        {
            entity = m_Entity;
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            ro1 = new RefRO<TRO1>(in Unsafe.AsRef<TRO1>(m_RO1));
        }
    }
}

public ref struct QueryRefEntityBuilder2<TW1, TW2>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefEntityBuilder2(QueryRefFilters filters)
    {
        m_Filters = filters;
    }

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly ulong[]  m_EntityIds;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_EntityIds, m_Count) = QueryRefBuffers.OpenWithEntities(ref filters, poolCount: 2);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 2;
                return new Row(
                    new EntityRef(m_EntityIds[m_Index]),
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TW2*)m_Buffer[baseIdx + 1]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly EntityRef m_Entity;
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;

        internal unsafe Row(EntityRef entity, TW1* w1, TW2* w2)
        {
            m_Entity = entity;
            m_W1 = w1;
            m_W2 = w2;
        }

        public EntityRef Entity => m_Entity;
        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);

        public unsafe void Deconstruct(out EntityRef entity, out Ref<TW1> w1, out Ref<TW2> w2)
        {
            entity = m_Entity;
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
        }
    }
}

public ref struct QueryRefEntityBuilder3<TW1, TW2, TW3>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
    where TW3 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefEntityBuilder3(QueryRefFilters filters)
    {
        m_Filters = filters;
    }

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly ulong[]  m_EntityIds;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_EntityIds, m_Count) = QueryRefBuffers.OpenWithEntities(ref filters, poolCount: 3);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 3;
                return new Row(
                    new EntityRef(m_EntityIds[m_Index]),
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TW2*)m_Buffer[baseIdx + 1],
                    (TW3*)m_Buffer[baseIdx + 2]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly EntityRef m_Entity;
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;

        internal unsafe Row(EntityRef entity, TW1* w1, TW2* w2, TW3* w3)
        {
            m_Entity = entity;
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
        }

        public EntityRef Entity => m_Entity;
        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);

        public unsafe void Deconstruct(out EntityRef entity, out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3)
        {
            entity = m_Entity;
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
            w3 = new Ref<TW3>(ref Unsafe.AsRef<TW3>(m_W3));
        }
    }
}

public ref struct QueryRefEntityBuilder4<TW1, TW2, TW3, TW4>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
    where TW3 : unmanaged, IComponent
    where TW4 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefEntityBuilder4(QueryRefFilters filters)
    {
        m_Filters = filters;
    }

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly ulong[]  m_EntityIds;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_EntityIds, m_Count) = QueryRefBuffers.OpenWithEntities(ref filters, poolCount: 4);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 4;
                return new Row(
                    new EntityRef(m_EntityIds[m_Index]),
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TW2*)m_Buffer[baseIdx + 1],
                    (TW3*)m_Buffer[baseIdx + 2],
                    (TW4*)m_Buffer[baseIdx + 3]);
            }
        }
    }

    public readonly ref struct Row
    {
        private readonly EntityRef m_Entity;
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;
        private readonly unsafe TW4* m_W4;

        internal unsafe Row(EntityRef entity, TW1* w1, TW2* w2, TW3* w3, TW4* w4)
        {
            m_Entity = entity;
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
            m_W4 = w4;
        }

        public EntityRef Entity => m_Entity;
        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);
        public unsafe ref TW4 W4 => ref Unsafe.AsRef<TW4>(m_W4);

        public unsafe void Deconstruct(out EntityRef entity, out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3, out Ref<TW4> w4)
        {
            entity = m_Entity;
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
            w3 = new Ref<TW3>(ref Unsafe.AsRef<TW3>(m_W3));
            w4 = new Ref<TW4>(ref Unsafe.AsRef<TW4>(m_W4));
        }
    }
}

public ref struct QueryRefEntityBuilder5<TW1, TW2, TW3, TW4, TW5>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
    where TW3 : unmanaged, IComponent
    where TW4 : unmanaged, IComponent
    where TW5 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefEntityBuilder5(QueryRefFilters filters)
    {
        m_Filters = filters;
    }

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly ulong[]  m_EntityIds;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_EntityIds, m_Count) = QueryRefBuffers.OpenWithEntities(ref filters, poolCount: 5);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 5;
                return new Row(
                    new EntityRef(m_EntityIds[m_Index]),
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
        private readonly EntityRef m_Entity;
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;
        private readonly unsafe TW4* m_W4;
        private readonly unsafe TW5* m_W5;

        internal unsafe Row(EntityRef entity, TW1* w1, TW2* w2, TW3* w3, TW4* w4, TW5* w5)
        {
            m_Entity = entity;
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
            m_W4 = w4;
            m_W5 = w5;
        }

        public EntityRef Entity => m_Entity;
        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);
        public unsafe ref TW4 W4 => ref Unsafe.AsRef<TW4>(m_W4);
        public unsafe ref TW5 W5 => ref Unsafe.AsRef<TW5>(m_W5);

        public unsafe void Deconstruct(out EntityRef entity, out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3, out Ref<TW4> w4, out Ref<TW5> w5)
        {
            entity = m_Entity;
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
            w3 = new Ref<TW3>(ref Unsafe.AsRef<TW3>(m_W3));
            w4 = new Ref<TW4>(ref Unsafe.AsRef<TW4>(m_W4));
            w5 = new Ref<TW5>(ref Unsafe.AsRef<TW5>(m_W5));
        }
    }
}

public ref struct QueryRefEntityBuilder6<TW1, TW2, TW3, TW4, TW5, TW6>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
    where TW3 : unmanaged, IComponent
    where TW4 : unmanaged, IComponent
    where TW5 : unmanaged, IComponent
    where TW6 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefEntityBuilder6(QueryRefFilters filters)
    {
        m_Filters = filters;
    }

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly ulong[]  m_EntityIds;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_EntityIds, m_Count) = QueryRefBuffers.OpenWithEntities(ref filters, poolCount: 6);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 6;
                return new Row(
                    new EntityRef(m_EntityIds[m_Index]),
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
        private readonly EntityRef m_Entity;
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;
        private readonly unsafe TW4* m_W4;
        private readonly unsafe TW5* m_W5;
        private readonly unsafe TW6* m_W6;

        internal unsafe Row(EntityRef entity, TW1* w1, TW2* w2, TW3* w3, TW4* w4, TW5* w5, TW6* w6)
        {
            m_Entity = entity;
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
            m_W4 = w4;
            m_W5 = w5;
            m_W6 = w6;
        }

        public EntityRef Entity => m_Entity;
        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);
        public unsafe ref TW4 W4 => ref Unsafe.AsRef<TW4>(m_W4);
        public unsafe ref TW5 W5 => ref Unsafe.AsRef<TW5>(m_W5);
        public unsafe ref TW6 W6 => ref Unsafe.AsRef<TW6>(m_W6);

        public unsafe void Deconstruct(out EntityRef entity, out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3, out Ref<TW4> w4, out Ref<TW5> w5, out Ref<TW6> w6)
        {
            entity = m_Entity;
            w1 = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            w2 = new Ref<TW2>(ref Unsafe.AsRef<TW2>(m_W2));
            w3 = new Ref<TW3>(ref Unsafe.AsRef<TW3>(m_W3));
            w4 = new Ref<TW4>(ref Unsafe.AsRef<TW4>(m_W4));
            w5 = new Ref<TW5>(ref Unsafe.AsRef<TW5>(m_W5));
            w6 = new Ref<TW6>(ref Unsafe.AsRef<TW6>(m_W6));
        }
    }
}

public ref struct QueryRefEntityBuilder7<TW1, TW2, TW3, TW4, TW5, TW6, TW7>
    where TW1 : unmanaged, IComponent
    where TW2 : unmanaged, IComponent
    where TW3 : unmanaged, IComponent
    where TW4 : unmanaged, IComponent
    where TW5 : unmanaged, IComponent
    where TW6 : unmanaged, IComponent
    where TW7 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefEntityBuilder7(QueryRefFilters filters)
    {
        m_Filters = filters;
    }

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly ulong[]  m_EntityIds;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_EntityIds, m_Count) = QueryRefBuffers.OpenWithEntities(ref filters, poolCount: 7);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 7;
                return new Row(
                    new EntityRef(m_EntityIds[m_Index]),
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
        private readonly EntityRef m_Entity;
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;
        private readonly unsafe TW4* m_W4;
        private readonly unsafe TW5* m_W5;
        private readonly unsafe TW6* m_W6;
        private readonly unsafe TW7* m_W7;

        internal unsafe Row(EntityRef entity, TW1* w1, TW2* w2, TW3* w3, TW4* w4, TW5* w5, TW6* w6, TW7* w7)
        {
            m_Entity = entity;
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
            m_W4 = w4;
            m_W5 = w5;
            m_W6 = w6;
            m_W7 = w7;
        }

        public EntityRef Entity => m_Entity;
        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);
        public unsafe ref TW4 W4 => ref Unsafe.AsRef<TW4>(m_W4);
        public unsafe ref TW5 W5 => ref Unsafe.AsRef<TW5>(m_W5);
        public unsafe ref TW6 W6 => ref Unsafe.AsRef<TW6>(m_W6);
        public unsafe ref TW7 W7 => ref Unsafe.AsRef<TW7>(m_W7);

        public unsafe void Deconstruct(out EntityRef entity, out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3, out Ref<TW4> w4, out Ref<TW5> w5, out Ref<TW6> w6, out Ref<TW7> w7)
        {
            entity = m_Entity;
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

public ref struct QueryRefEntityBuilder8<TW1, TW2, TW3, TW4, TW5, TW6, TW7, TW8>
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

    internal QueryRefEntityBuilder8(QueryRefFilters filters)
    {
        m_Filters = filters;
    }

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly ulong[]  m_EntityIds;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_EntityIds, m_Count) = QueryRefBuffers.OpenWithEntities(ref filters, poolCount: 8);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 8;
                return new Row(
                    new EntityRef(m_EntityIds[m_Index]),
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
        private readonly EntityRef m_Entity;
        private readonly unsafe TW1* m_W1;
        private readonly unsafe TW2* m_W2;
        private readonly unsafe TW3* m_W3;
        private readonly unsafe TW4* m_W4;
        private readonly unsafe TW5* m_W5;
        private readonly unsafe TW6* m_W6;
        private readonly unsafe TW7* m_W7;
        private readonly unsafe TW8* m_W8;

        internal unsafe Row(EntityRef entity, TW1* w1, TW2* w2, TW3* w3, TW4* w4, TW5* w5, TW6* w6, TW7* w7, TW8* w8)
        {
            m_Entity = entity;
            m_W1 = w1;
            m_W2 = w2;
            m_W3 = w3;
            m_W4 = w4;
            m_W5 = w5;
            m_W6 = w6;
            m_W7 = w7;
            m_W8 = w8;
        }

        public EntityRef Entity => m_Entity;
        public unsafe ref TW1 W1 => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref TW2 W2 => ref Unsafe.AsRef<TW2>(m_W2);
        public unsafe ref TW3 W3 => ref Unsafe.AsRef<TW3>(m_W3);
        public unsafe ref TW4 W4 => ref Unsafe.AsRef<TW4>(m_W4);
        public unsafe ref TW5 W5 => ref Unsafe.AsRef<TW5>(m_W5);
        public unsafe ref TW6 W6 => ref Unsafe.AsRef<TW6>(m_W6);
        public unsafe ref TW7 W7 => ref Unsafe.AsRef<TW7>(m_W7);
        public unsafe ref TW8 W8 => ref Unsafe.AsRef<TW8>(m_W8);

        public unsafe void Deconstruct(out EntityRef entity, out Ref<TW1> w1, out Ref<TW2> w2, out Ref<TW3> w3, out Ref<TW4> w4, out Ref<TW5> w5, out Ref<TW6> w6, out Ref<TW7> w7, out Ref<TW8> w8)
        {
            entity = m_Entity;
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
