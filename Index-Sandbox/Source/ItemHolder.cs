using Index;

// Demonstrates referencing a DataAsset from a script: assign an Item Data asset in
// the inspector (the field shows a Data Asset picker), or it resolves from a saved
// scene on load. One DataAsset can reference another the same way.
public class ItemHolder : EntityScript
{
    public ItemData? Item;
    public int Count = 1;

    public override void OnStart()
    {
        if (Item != null)
            Log.Info($"ItemHolder: {Count}x {Item.DisplayName} (weight {Item.Weight}, max stack {Item.MaxStack})");
        else
            Log.Warn("ItemHolder has no Item assigned.");
    }
}
