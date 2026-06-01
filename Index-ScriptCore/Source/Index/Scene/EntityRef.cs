namespace Index;

/// <summary>Opaque handle into an ECB's entity table. Valid only within the originating ECB and only before <c>Playback</c> runs.</summary>
public readonly struct EntityRef
{
    public readonly uint Index;

    internal EntityRef(uint index)
    {
        Index = index;
    }
}
