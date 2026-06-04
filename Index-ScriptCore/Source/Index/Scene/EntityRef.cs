namespace Index;

/// <summary>Lightweight entity reference. ECB refs use <see cref="Index"/>; live scene refs use <see cref="ID"/>.</summary>
public readonly struct EntityRef
{
    public readonly ulong ID;
    public readonly uint Index;

    internal EntityRef(uint index)
    {
        ID = 0;
        Index = index;
    }

    internal EntityRef(ulong id)
    {
        ID = id;
        Index = 0;
    }

    public Entity Entity => ID != 0 ? new Entity(ID) : Entity.Invalid;
    public bool IsSceneEntity => ID != 0;
    public bool IsCommandBufferEntity => ID == 0;
}
