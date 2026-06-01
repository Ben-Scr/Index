namespace Index.Components;

// Ref<T>/RefRO<T> exist because C# tuple deconstruct can't yield `ref` locals directly; wrapping in a ref struct lets the local copy still reach the underlying pool slot via .Value.
public readonly ref struct Ref<T> where T : unmanaged, IComponent
{
    private readonly ref T m_Ref;
    public ref T Value => ref m_Ref;
    internal Ref(ref T r) { m_Ref = ref r; }
}

public readonly ref struct RefRO<T> where T : unmanaged, IComponent
{
    private readonly ref readonly T m_Ref;
    public ref readonly T Value => ref m_Ref;
    internal RefRO(in T r) { m_Ref = ref r; }
}
