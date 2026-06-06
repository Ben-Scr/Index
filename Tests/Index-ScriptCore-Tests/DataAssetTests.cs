using System;
using System.Reflection;
using Index;
using Xunit;

namespace IndexScriptCoreTests;

// Pins the public DataAsset authoring contract. The runtime side
// (DataAssetRuntime / InternalCalls) is internal and needs the native host, so
// it lives in the engine tests; these cover what a script author actually sees.

public class CreateDataAssetAttributeTests
{
    [Fact]
    public void DefaultMenuPath_IsEmpty()
    {
        // No path => usable as a DataAsset but absent from the "Create" menu.
        Assert.Equal(string.Empty, new CreateDataAssetAttribute().MenuPath);
    }

    [Fact]
    public void MenuPath_IsPreserved()
    {
        Assert.Equal("Gameplay/Item Data", new CreateDataAssetAttribute("Gameplay/Item Data").MenuPath);
    }

    [Fact]
    public void Usage_IsClassOnly_NotInheritedOrMultiple()
    {
        AttributeUsageAttribute? usage =
            typeof(CreateDataAssetAttribute).GetCustomAttribute<AttributeUsageAttribute>();
        Assert.NotNull(usage);
        Assert.Equal(AttributeTargets.Class, usage!.ValidOn);
        Assert.False(usage.Inherited);   // a subclass must opt in explicitly
        Assert.False(usage.AllowMultiple);
    }

    [Fact]
    public void Attribute_IsDiscoverableViaReflection()
    {
        // This is exactly how the editor enumerates "Create > Data Asset" entries.
        CreateDataAssetAttribute? attr =
            typeof(SampleItemData).GetCustomAttribute<CreateDataAssetAttribute>();
        Assert.NotNull(attr);
        Assert.Equal("Gameplay/Item Data", attr!.MenuPath);
    }
}

public class DataAssetTests
{
    [Fact]
    public void DataAsset_IsAbstract()
    {
        // The base class is authoring scaffolding only; instances are subclasses.
        Assert.True(typeof(DataAsset).IsAbstract);
    }

    [Fact]
    public void OnValidate_DefaultIsNoOp()
    {
        DataAsset asset = new PlainDataAsset();
        Assert.Null(Record.Exception(() => asset.OnValidate()));
    }

    [Fact]
    public void OnValidate_OverrideRuns_AndCanClampFields()
    {
        SampleItemData asset = new SampleItemData { Damage = -5f };
        asset.OnValidate();
        Assert.True(asset.Validated);
        Assert.Equal(0f, asset.Damage); // override clamps the negative away
    }
}

[CreateDataAsset("Gameplay/Item Data")]
internal sealed class SampleItemData : DataAsset
{
    public string DisplayName = "Sword";
    public float Damage = 10f;
    public bool Validated;

    public override void OnValidate()
    {
        Validated = true;
        if (Damage < 0f)
            Damage = 0f;
    }
}

internal sealed class PlainDataAsset : DataAsset
{
}
