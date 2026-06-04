# Scripting

Index uses **C#** as its primary scripting language (running on **.NET 9**). You write scripts to give your game behavior — move the player, spawn enemies, handle UI, manage game state. Index also supports **native C++ scripts** for performance-critical code.

If you just want the short version: make a class that inherits from `EntityScript`, override `OnUpdate`, attach it to an entity in the editor, and press Play.

\---

## The three kinds of script

Index has three script base classes, each with a different **scope**:

|Base class|Scope|Use it for|
|-|-|-|
|**EntityScript**|One entity|Behavior for a specific object — a player, an enemy, a pickup.|
|**SceneScript**|One scene|Logic over a whole scene — spawning waves, scoring, scene-wide rules.|
|**GlobalScript**|The whole app|State that survives scene changes — a game manager, audio settings, save data.|

Pick the smallest scope that fits. Most gameplay is `EntityScript`; reach for `SceneScript` when you need to work with *many* entities at once, and `GlobalScript` for things that must persist between scenes.

\---

## Your first script

Here's a complete `EntityScript` that moves its entity with the arrow keys / WASD:

```csharp
using Index;

public class PlayerController : EntityScript
{
    private float speed = 5.0f;

    public override void OnStart()
    {
        Print("PlayerController attached to: " + Entity.Name);
    }

    public override void OnUpdate()
    {
        var velocity = Vector2.Zero;

        if (Input.GetKey(KeyCode.W)) velocity.Y += 1.0f;
        if (Input.GetKey(KeyCode.S)) velocity.Y -= 1.0f;
        if (Input.GetKey(KeyCode.A)) velocity.X -= 1.0f;
        if (Input.GetKey(KeyCode.D)) velocity.X += 1.0f;

        if (velocity == Vector2.Zero) return;

        Entity.Transform.Position += velocity.Normalized() \* speed \* Time.DeltaTime;
    }
}
```

Notice three things:

* It inherits from `EntityScript`.
* `Entity` refers to the object this script is attached to.
* `Input`, `Time`, and `Transform` are part of the scripting API (covered below).

### Attaching a script to an entity

A script doesn't run until it's attached. In the editor, select an entity, **Add Component → Script** (a `ScriptComponent`), and choose your script class from the list. The engine finds your class, creates an instance, and starts calling its lifecycle methods when the game plays.

\---

## Lifecycle methods

The engine calls these methods at the right moments — you override the ones you need. The exact set varies slightly by script type.

**EntityScript** (per entity):

```csharp
public override void OnAwake() { }          // created, before OnStart
public override void OnStart() { }           // first frame, after everything is loaded
public override void OnUpdate() { }          // every frame
public override void OnFixedUpdate() { }     // every fixed (physics) step
public override void OnDestroy() { }         // entity/script destroyed
public override void OnEnable() { }          // enabled
public override void OnDisable() { }         // disabled

// Physics collisions (need a Rigidbody2D + collider):
public override void OnCollisionEnter2D(Collision2D c) { }
public override void OnCollisionStay2D(Collision2D c) { }
public override void OnCollisionExit2D(Collision2D c) { }
```

**SceneScript** (per scene) has the same `OnAwake` / `OnStart` / `OnUpdate` / `OnFixedUpdate` / `OnDestroy` / `OnEnable` / `OnDisable`, plus a `Scene` property for querying entities — but no collision callbacks.

**GlobalScript** (whole app) starts with `OnInitialize()` instead of Awake/Start, then `OnUpdate` / `OnFixedUpdate`. It is created at startup and persists across scene loads.

All three also receive application events like `OnApplicationPaused`, `OnApplicationQuit`, and `OnFocusChanged(bool focused)`.

> The split between `OnUpdate` and `OnFixedUpdate` matters: put \*\*rendering/input-rate\*\* logic in `OnUpdate`, and \*\*physics\*\* logic in `OnFixedUpdate` (which runs at a steady rate regardless of frame rate). See \[Startup Processes](../Engine-Core/Startup-Processes.md#stage-3--the-main-loop).

\---

## Exposing fields to the Inspector

Public fields can be shown and edited in the editor's Inspector using **attributes**. This lets designers tweak values without touching code:

```csharp
public class Enemy : EntityScript
{
    \[Header("Stats")]
    \[ShowInEditor("Move Speed")] public float Speed = 3.0f;
    \[ClampValue(0f, 100f)]       public float Health = 100f;

    \[Space(8f)]
    \[ToolTip("Seconds between attacks")]
    public float AttackCooldown = 1.5f;

    \[EnabledIf("UseCustomColor", true)] public Color Tint = Color.White;
    public bool UseCustomColor = false;

    \[HideFromEditor] public int internalCounter;  // public but hidden
}
```

The main attributes:

|Attribute|Effect|
|-|-|
|`\[ShowInEditor("Label")]`|Show the field (optionally with a custom label, and read-only if you want).|
|`\[ClampValue(min, max)]`|Limit a numeric field to a range.|
|`\[Header("Text")]`|A bold section header above the field.|
|`\[ToolTip("Text")]`|Hover text explaining the field.|
|`\[Space(pixels)]`|Vertical gap for grouping.|
|`\[EnabledIf("Field", value)]`|Only enable this field when another field has a given value.|
|`\[EditorReadOnly]`|Visible but not editable.|
|`\[HideFromEditor]`|Hide a public field.|
|`\[EditorIgnore]`|Hide all fields of a whole type.|

You can also expose **asset slots** (a texture, audio clip, etc.) using reference types — drag an asset onto the slot in the Inspector. See [Assets](../Assets/Assets.md#referencing-assets-from-c-scripts).

\---

## What scripts can do (the API)

The `Index` namespace gives you a broad API. The main categories:

**Entities \& components**

```csharp
Entity e = Entity.FindByName("Boss");
e.GetComponent<SpriteRenderer>();
e.AddComponent<Rigidbody2D>();
Entity copy = Entity.Instantiate(prefab);
e.Destroy();
```

**Transform** — position, rotation, scale, and hierarchy:

```csharp
Entity.Transform.Position += Vector2.Up \* Time.DeltaTime;
Entity.Transform.SetParent(other.Transform);
```

**Input** — keyboard, mouse, axes:

```csharp
Input.GetKey(KeyCode.Space);        // held
Input.GetKeyDown(KeyCode.Space);    // pressed this frame
Input.GetAxis();                    // smoothed WASD/arrows, -1..1
Input.MousePosition;
```

**Time**:

```csharp
Time.DeltaTime;        // seconds since last frame (scaled)
Time.FixedDeltaTime;   // physics step length
Time.TimeScale = 0.5f; // slow motion (or 0 to pause game time)
```

**Physics 2D** — raycasts and overlap checks:

```csharp
RaycastHit2D hit = Physics2D.Raycast(origin, direction, maxDistance);
Entity? hitEntity = Physics2D.OverlapCircle(center, radius);
```

**Audio**:

```csharp
Audio sfx = Audio.FromAssetUUID(clipGuid);
sfx.PlayOneShot();
```

**Logging** — prints to the editor Console:

```csharp
Log.Info("Hello");
Log.Warn("Careful");
Log.Error("Something broke");
```

**Scene queries** — process many entities at once (great in a `SceneScript`):

```csharp
foreach (ref var t in Scene.QueryRef<NativeTransform2D>())
    t.LocalRotation += Time.DeltaTime;      // spin everything
```

**Math** — `Vector2/3/4`, `Quaternion`, `Color`, `Mathf` (sin, cos, clamp, lerp…), and `Random`.

For heavy parallel work over many entities, combine queries with the [Job System](../Engine-Core/Job-System.md).

\---

## A scene-wide script

When you need to operate on lots of entities, a `SceneScript` with a query is the natural tool:

```csharp
using Index;
using Index.Native;

public class SpinSystem : SceneScript
{
    \[ShowInEditor("Spin Speed")] public float SpinSpeed = 1.0f;

    public override void OnUpdate()
    {
        float dt = Time.DeltaTime;
        foreach (ref var t in Scene.QueryRef<NativeTransform2D>())
            t.LocalRotation += SpinSpeed \* dt;
    }
}
```

`QueryRef<T>` gives you a direct reference to each entity's component, so you can read and write it with no copying — ideal for hot loops.

\---

## Native C++ scripts

For performance-critical code, you can write a **native script** in C++ instead. A native script inherits from `NativeScript` and registers itself with a macro:

```cpp
class MyScript : public Index::NativeScript {
public:
    void Start() override { /\* ... \*/ }
    void Update(float deltaTime) override { /\* ... \*/ }
    void OnDestroy() override { /\* ... \*/ }
};

REGISTER\_SCRIPT(MyScript);   // makes it selectable like a C# script
```

Differences from C#:

* **Compiled, not hot-reloaded** — changing a native script means rebuilding.
* **Faster** — direct memory access, no garbage collection.
* **Smaller lifecycle** — `Start`, `Update(deltaTime)`, and `OnDestroy` (no collision/enable/application hooks).

Use C# for almost everything; drop to native scripts only when you've measured a real need.

\---

## Hot reload

A big convenience of C# scripting: you can change your scripts and reload them **without restarting** the editor. When you rebuild your game's script assembly, the engine swaps in the new code and keeps your scenes running. Entity state is preserved where it can be (matching script classes keep their instances; fields reset to their default/saved values).

The engine's *core* scripting library can't be reloaded mid-session (a .NET limitation), so changing engine-level scripting bits needs a restart — but your everyday game scripts reload freely.

\---

## How it works under the hood (brief)

You don't need this to write scripts, but it helps to know the shape:

* At startup, the engine boots the **.NET runtime** (via hostfxr) and loads two assemblies: **`Index-ScriptCore.dll`** (the scripting API you call) and **your game's script assembly**.
* A bridge connects the two worlds: C# calls into the engine through **InternalCalls** (P/Invoke), and the engine calls back into your scripts (e.g. to invoke `OnUpdate`) through a table of function pointers.
* This bridge is set up once during the scripting-host startup step described in [Startup Processes](../Engine-Core/Startup-Processes.md#start-scripting).

\---

## Summary

* Write game logic in **C#**: `EntityScript` (per entity), `SceneScript` (per scene), or `GlobalScript` (whole app).
* Override lifecycle methods (`OnStart`, `OnUpdate`, `OnFixedUpdate`, …); attach scripts via a `ScriptComponent`.
* Expose fields to the Inspector with attributes like `\[ShowInEditor]` and `\[ClampValue]`.
* Use the rich API — `Input`, `Time`, `Physics2D`, `Audio`, `Log`, scene queries, math.
* Drop to **native C++ scripts** only for measured performance needs.
* **Hot reload** lets you iterate on C# without restarting.

## Related pages

* [ECS](../Engine-Core/ECS.md) — entities and components your scripts work with.
* [Scenes](../Engine-Core/Scenes.md) — SceneScripts and the play-mode lifecycle.
* [Assets](../Assets/Assets.md) — referencing textures, audio, and fonts from scripts.
* [Job System](../Engine-Core/Job-System.md) — parallelizing heavy script work.

