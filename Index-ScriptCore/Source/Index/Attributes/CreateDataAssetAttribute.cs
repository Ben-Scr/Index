using System;

namespace Index;

/// <summary>
/// Places a <see cref="DataAsset"/> subclass in the editor's "Create &gt; Data Asset"
/// menu. The path is slash-separated for submenus, e.g. "Gameplay/Item Data".
/// Without it, the subclass is still usable but won't appear in the create menu.
/// </summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
public sealed class CreateDataAssetAttribute : Attribute
{
    public string MenuPath { get; }

    public CreateDataAssetAttribute(string menuPath = "")
    {
        MenuPath = menuPath;
    }
}
