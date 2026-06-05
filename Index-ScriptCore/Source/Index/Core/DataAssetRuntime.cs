using System;
using Index.Interop;

namespace Index;

// Resolves a DataAsset reference (held as a GUID in a serialized field) to its
// live instance. The native DataAssetManager is the single owner of loaded
// instances; this asks it to ensure the asset is loaded, then fetches the
// managed object the host instantiated for it.
internal static class DataAssetRuntime
{
    internal static DataAsset? LoadByGuid(ulong guid, Type subtype)
    {
        if (guid == 0 || !InternalCalls.DataAsset_EnsureLoaded(guid))
            return null;

        object? instance = ScriptInstanceManager.GetDataAssetObject(guid);
        // Reject a GUID that resolves to a different DataAsset subtype than the
        // field expects, rather than throwing on the SetValue.
        return subtype.IsInstanceOfType(instance) ? (DataAsset)instance! : null;
    }
}
