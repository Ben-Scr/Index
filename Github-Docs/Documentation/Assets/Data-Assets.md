# Data Assets

A **Data Asset** is a reusable, shared piece of game data that lives on its own — not attached to any entity or scene. Item definitions, character stats, loot tables, dialogue, difficulty settings: anything you'd want to author once and reference from many places.

If you've used Unity, this is Index's equivalent of a **ScriptableObject**.

If you just want the short version: write a C# class that inherits from `DataAsset`, create instances of it in the editor as `.dataasset` files, fill in their fields, and reference them from your scripts.

---

## Why use them?

Imagine 50 enemies that share the same stats. You *could* copy those numbers onto every enemy — but then changing one value means editing 50 entities. Instead, put the data in a single **Data Asset** and have every enemy reference it. Change the asset once, and everyone updates.

Data Assets are good for:

- **Definitions** — items, weapons, spells, enemy types.
- **Tuning data** — stats, curves, difficulty presets.
- **Content** — dialogue lines, quest descriptions, loot tables.
- **Shared config** — anything multiple objects should read from one source of truth.

The key idea: the data is **decoupled** from entities and scenes. One asset, referenced anywhere, edited in one place.

---

## How they work

A Data Asset has two halves:

1. **A C# class** you write (a subclass of `DataAsset`) — this defines *what fields the data has*.
2. **`.dataasset` files** — individual instances of that class, each saved with a permanent **GUID**, just like any other [asset](Assets.md).

So the class is the *template*, and each `.dataasset` file is one filled-in copy. `ItemData` is a class; `Sword.dataasset` and `Potion.dataasset` are instances of it.

At runtime, the engine keeps **one shared live instance** per `.dataasset` file. Everything that references `Sword.dataasset` gets the *same* object — so they all see the same values, and there's no duplication. References are stored as the asset's GUID (not a copy of the data), exactly like texture and audio references.

> Data Assets are part of the **scripting layer**, not the engine core. A build with scripting stripped out simply doesn't include them.

---

## Authoring a Data Asset class

Write a class that inherits from `DataAsset` and add `[CreateDataAsset(...)]` so it shows up in the editor's create menu. Here's the sample from the Sandbox:

```csharp
using Index;

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
            MaxStack = 1;   // keep the value sane after edits
    }
}
```

A few things to notice:

- **`[CreateDataAsset("Gameplay/Item Data")]`** puts an entry in the editor's *Create → Data Asset* menu. The path is slash-separated for submenus. Without this attribute the class still works, but it won't appear in the create menu.
- **Fields** use the same rules and [attributes](../Scripting/Scripting.md#exposing-fields-to-the-inspector) as script fields — `[ClampValue]`, `[ShowInEditor]`, `[Header]`, `[ToolTip]`, and so on. Public fields are editable by default.
- **`OnValidate()`** runs after the fields are edited in the editor or loaded from disk. Override it to clamp or recompute values — it's the analogue of Unity's `OnValidate`.

### What field types are allowed?

The same set as script fields:

- primitives (`int`, `float`, `bool`, …) and `string`,
- enums,
- the built-in math/color types (`Vector2/3/4`, `Color`, …),
- asset references like `Texture` and `Audio`,
- and **other `DataAsset` subclasses** — a Data Asset can reference another Data Asset (e.g. an `ItemData` that points at a `RarityData`).

---

## Creating and editing instances (editor)

Once your class exists and the project is built:

1. In the **Asset Browser**, right-click → **Create → Data Asset → Gameplay/Item Data** (your menu path).
2. A new `.dataasset` file appears. Name it (e.g. `Sword`).
3. Select it — its fields show in the **Inspector**, starting from the class's default values.
4. Edit the values. They're saved to the `.dataasset` file.

Each file is an independent instance with its own values, but all share the class you wrote. See [Assets › The Asset Browser](Assets.md#the-asset-browser-editor).

---

## Referencing a Data Asset from a script

Just declare a field of your Data Asset type. The editor shows a picker so you can assign an asset by drag-and-drop, and the reference is saved (by GUID) with the scene:

```csharp
using Index;

public class ItemHolder : EntityScript
{
    public ItemData? Item;   // shows a Data Asset picker in the Inspector
    public int Count = 1;

    public override void OnStart()
    {
        if (Item != null)
            Log.Info($"{Count}x {Item.DisplayName} (weight {Item.Weight})");
        else
            Log.Warn("ItemHolder has no Item assigned.");
    }
}
```

Because every holder that points at the same asset shares one instance, reading `Item.DisplayName` always reflects the asset's current values. (And as noted above, one Data Asset can reference another the same way.)

---

## Loading a Data Asset from code

You don't have to assign assets in the Inspector — you can load them at runtime through the [`AssetManager`](Assets.md#referencing-assets-from-c-scripts):

```csharp
// Load a specific one by its project path:
ItemData? sword = AssetManager.Load<ItemData>("Items/Sword.dataasset");

// Or get every ItemData in the project (optionally under a folder):
IReadOnlyList<ItemData> allItems = AssetManager.FindAll<ItemData>();
IReadOnlyList<ItemData> weapons  = AssetManager.FindAll<ItemData>("Items/Weapons");
```

`FindAll<T>` is handy for things like building a catalog of every item at startup.

---

## What's in a `.dataasset` file

A `.dataasset` file is small and self-describing. It stores:

- the **type** name of the class it's an instance of (e.g. `"ItemData"`),
- a format **version**, and
- the **field values**.

Like scenes and prefabs, it's written as text (JSON) or in a compact binary form depending on your project's asset-serialization setting. The file on disk is always the source of truth; the engine loads it into the shared live instance on demand.

---

## Hot reload

Data Assets play nicely with C# [hot reload](../Scripting/Scripting.md#hot-reload). When you rebuild your scripts, the live managed instances are swapped out and then **re-created from disk**. The engine creates every instance first and *then* replays their field values, so cross-references between Data Assets resolve correctly even when assets point at each other.

In practice this means you can tweak a Data Asset class, reload, and keep working — your `.dataasset` files and their values are preserved.

---

## Data Asset vs. the alternatives

It helps to know when to reach for a Data Asset versus something else:

| Use… | When you want… |
|------|----------------|
| **Data Asset** | Shared data, decoupled from any entity — one source of truth referenced from many places. |
| **Prefab** | A reusable *entity* (with components, children, transforms) you stamp into scenes. See [Scenes › Prefabs](../Engine-Core/Scenes.md#prefabs). |
| **Plain script fields** | Per-instance values that belong to one specific entity and aren't shared. See [Scripting](../Scripting/Scripting.md). |

Rule of thumb: if several objects should read the **same** data and you want to edit it in **one** place, use a Data Asset.

---

## Summary

- A **Data Asset** is shared game data in its own file — Index's **ScriptableObject** equivalent.
- Write a `class X : DataAsset`; add `[CreateDataAsset("Menu/Path")]` to get a create-menu entry.
- Each instance is a `.dataasset` file with a permanent **GUID**; the engine keeps **one shared live instance** per file.
- Fields follow the script-field rules (primitives, strings, enums, vectors/colors, `Texture`/`Audio`, and other Data Assets); `OnValidate()` runs after edits.
- Create and edit them in the **Asset Browser/Inspector**; reference them from scripts by declaring a field, or load them with `AssetManager.Load<T>` / `FindAll<T>`.

## Related pages

- [Assets](Assets.md) — the wider asset system Data Assets are part of (GUIDs, the Asset Browser, loading).
- [Scripting](../Scripting/Scripting.md) — the C# layer, field attributes, and hot reload.
- [Scenes](../Engine-Core/Scenes.md) — prefabs, the entity-shaped alternative.
