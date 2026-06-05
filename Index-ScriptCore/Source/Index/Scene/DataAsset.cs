namespace Index;

/// <summary>
/// Base class for a persistent, shared data asset — item definitions, stats,
/// loot tables, dialogue, and similar game data. A subclass instance is saved as
/// a <c>.dataasset</c> file with a stable GUID, decoupled from any entity or
/// scene. Author a subclass, expose fields with public access or
/// <see cref="ShowInEditorAttribute"/>, create and edit instances in the editor,
/// and reference them from scripts or from other data assets.
/// <para>
/// Field types follow the same rule as script fields: primitives, strings,
/// enums, the built-in vector/color types, asset references (Texture, Audio),
/// and other <see cref="DataAsset"/> subclasses.
/// </para>
/// </summary>
public abstract class DataAsset
{
    // The asset GUID this instance was loaded from; doubles as its key in the
    // host's instance table. Set by the host right after construction.
    internal ulong UUID;

    internal void _SetUUID(ulong uuid) => UUID = uuid;

    /// <summary>
    /// Called after the asset's fields are edited in the editor or replayed from
    /// disk. Override to clamp or derive values (the analogue of Unity's OnValidate).
    /// </summary>
    public virtual void OnValidate() { }
}
