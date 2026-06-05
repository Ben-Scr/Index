using Index;

// Sample DataAsset: a reusable item definition. Create instances from the editor
// (right-click in the Asset Browser -> Create -> Data Asset -> Gameplay/Item Data),
// edit their fields in the inspector, reference them from a script (see ItemHolder),
// or load them in code with AssetManager.Load<ItemData>(path) / FindAll<ItemData>().
[CreateDataAsset("Gameplay/Item Data")]
public class ItemData : DataAsset
{
    public string DisplayName = "New Item";
    public int MaxStack = 99;
    [ClampValue(0f, 1000f)] public float Weight = 1.0f;
    public Color Tint = new Color(1f, 1f, 1f, 1f);
    public Texture? Icon;

    public override void OnValidate()
    {
        if (MaxStack < 1)
            MaxStack = 1;
    }
}
