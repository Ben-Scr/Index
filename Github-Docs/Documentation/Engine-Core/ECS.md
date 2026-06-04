# ECS — Entities & Components

Index organizes everything in your game with an **Entity Component System (ECS)**. This page explains the model: what entities and components are, the components Index ships with, and how you work with them.

If you just want the short version: an **entity** is an empty "thing" in your scene. You give it **components** (data like a position, a sprite, a collider) to make it *something*. Code then reads and changes those components.

> Index uses the [EnTT](https://github.com/skypjack/entt) library under the hood, but you rarely touch it directly — Index wraps it in a friendly `Entity` type.

---

## The three pieces

ECS splits a game object into three separate ideas:

| Piece | What it is | Example |
|-------|-----------|---------|
| **Entity** | An ID — a lightweight handle for "a thing." Has no data of its own. | "Object #42" |
| **Component** | A bundle of data attached to an entity. | `Transform2D { position, rotation, scale }` |
| **System / Script** | Code that runs over entities with certain components. | "Move everything with a `Rigidbody2D`." |

The power of this split: an entity is whatever its components say it is. Add a `SpriteRenderer` and it draws; add a `Rigidbody2D` and it falls; remove them and it stops. You compose behavior instead of inheriting it.

---

## Entities

An **entity** is essentially an ID. In code, Index gives you an `Entity` handle that bundles that ID with the scene it belongs to, so you can call methods on it.

Creating and using an entity in C++:

```cpp
Entity e = Entity::Create("Player");      // make a named entity
e.AddComponent<SpriteRendererComponent>(); // give it a sprite
auto& tr = e.GetComponent<Transform2DComponent>();
tr.SetPosition({ 5.0f, 0.0f });
```

The same idea in a C# script:

```csharp
Entity e = Entity.Create("Player");
e.AddComponent<SpriteRenderer>();
e.Transform.Position = new Vector2(5f, 0f);
```

The core entity operations are the same everywhere:

- `AddComponent<T>()` / `RemoveComponent<T>()`
- `GetComponent<T>()` / `HasComponent<T>()`
- `Create(...)` / `Destroy()`
- parent/child: `SetParent(...)`, `GetParent()`, get children

### Entity identity

Entities carry a bit of bookkeeping so the engine knows where each one came from:

- a **name** (the `NameComponent`),
- a stable **UUID** for entities saved in a scene,
- an **origin**: was this entity authored in a **Scene**, spawned from a **Prefab**, or created at **Runtime**?

This is how the engine knows, for example, that runtime-spawned entities shouldn't be written back into your scene file.

---

## Components

A component is just **data**. In C++ they're plain structs — no base class required:

```cpp
struct HealthComponent {
    float Current = 100.0f;
    float Max     = 100.0f;
};
```

Some components hold no data at all — these are **tags**, used purely as markers (for example a `DisabledTag` that flags an entity as turned off). Code can quickly ask "give me every entity with this tag."

### Built-in components

Index ships a large set of components, grouped by area. Here are the main ones:

**General**
- `Transform2D` — position, rotation, and scale (the most fundamental component). Supports local *and* world space, and follows its parent in a hierarchy.
- `Name` — the entity's display name.
- `Hierarchy` — parent and children links.
- `RectTransform2D` — a UI-oriented transform for layout.

**Graphics**
- `SpriteRenderer` — draws a texture/sprite.
- `Camera2D` — a 2D camera (what the player sees).
- `TextRenderer` — draws text with a font.
- `Image` — a UI image.
- `ParticleSystem2D` — particle effects.
- `PostProcessing2D` — screen effects like bloom or vignette.

**Physics**
- `Rigidbody2D` — makes an entity participate in physics.
- `BoxCollider2D`, `CircleCollider2D`, `PolygonCollider2D` — collision shapes.

**UI**
- `Button`, `Slider`, `Toggle`, `InputField`, `Dropdown`, `ScrollRect`, layout groups, and more.

**Audio**
- `AudioSource` — plays a sound from an entity.

You add any of these in the editor through the **Add Component** menu, or in code with `AddComponent<T>()`.

### How components become "known"

For a component to appear in the editor and save/load with scenes, it must be **registered** — given a display name, a category, a serialized name, and a list of editable fields. The engine registers all its built-ins at startup; **packages register their own** the same way (see [Writing Packages](../Packages/Writing-Packages.md)).

Registration is also where the engine declares rules like "these two components can't be on the same entity at once" (conflicts), which the Add Component menu respects.

---

## Hierarchy (parents and children)

Entities can be nested. A child's `Transform2D` is **relative to its parent**, so moving a parent moves all its children with it. This is stored with the `Hierarchy` component (a parent link plus a list of children).

```csharp
child.SetParent(parent);   // child now follows parent
```

In the editor you build hierarchy by dragging entities onto each other in the Scene Hierarchy panel.

> **Scripting note:** when you move an entity by writing directly to a native transform reference, you may need to flag the transform as "dirty" so children update. The higher-level `Transform` API does this for you.

---

## How entities are stored

All the entities of a scene live in that scene's **registry** (the EnTT world). Each [Scene](Scenes.md) owns its own registry, so entities in one scene are completely separate from another's.

Components of the same type are stored together in tight arrays, which makes iterating over "every entity with component X" very fast.

---

## Working with many entities

Game logic usually runs over *all* entities that have a certain set of components. This is what **systems** (C++) and **scripts** (C#) do.

In C#, you query a scene for the combination you care about:

```csharp
// Every entity that has both a Transform2D and a SpriteRenderer:
foreach (ref var t in Scene.QueryRef<NativeTransform2D>())
{
    t.LocalRotation += Time.DeltaTime;  // spin everything
}
```

In C++, a system iterates a *view* of the registry:

```cpp
auto view = registry.view<Transform2DComponent, Rigidbody2DComponent>();
for (auto entity : view) {
    auto& tr = view.get<Transform2DComponent>(entity);
    // ...move it...
}
```

For details on **systems** (the C++ side, with `Awake`/`Start`/`Update`) see [Scenes](Scenes.md#systems). For **scripts** (the C# side) see [Scripting](../Scripting/Scripting.md). For running queries across many threads, see the [Job System](Job-System.md).

---

## Summary

- An **entity** is an ID; **components** are the data attached to it; **systems/scripts** are the code.
- Components are plain data; empty ones are **tags**.
- Index ships components for transforms, graphics, physics, UI, and audio; packages add more.
- Each **scene** owns its entities. Entities can form **parent/child** hierarchies.
- You work with entities via `AddComponent` / `GetComponent` and query many at once with views (C++) or `Query` (C#).

## Related pages

- [Scenes](Scenes.md) — the container entities live in, plus systems and their lifecycle.
- [Scripting](../Scripting/Scripting.md) — writing C# logic over entities and components.
- [Writing Packages](../Packages/Writing-Packages.md) — adding your own components.
- [Job System](Job-System.md) — processing many entities in parallel.
