# Index Documentation

Welcome to the documentation for **Index**, a 2D game engine. These pages explain how the engine works and how to build games with it, from launching a game to writing your own packages.

New here? A good reading order is **Editor → ECS → Scenes → Scripting**, then the rest as you need it.

---

## Engine Core

The foundational systems that make a game run.

- **[Startup Processes](Engine-Core/Startup-Processes.md)** — what happens, step by step, when a game launches: boot, load, and the main loop.
- **[ECS](Engine-Core/ECS.md)** — entities and components, the building blocks of everything in a scene.
- **[Scenes](Engine-Core/Scenes.md)** — scenes, systems, the scene lifecycle, saving, and prefabs.
- **[Job System](Engine-Core/Job-System.md)** — running work across multiple CPU cores.
- **[Localization](Engine-Core/Localization.md)** — translating interface text, the per-language JSON files, supported languages, and on-demand downloads.

## Assets

- **[Assets](Assets/Assets.md)** — how textures, audio, fonts, scenes, and prefabs are identified, found, referenced, and loaded.
- **[Data Assets](Assets/Data-Assets.md)** — shared, reusable game data (Index's ScriptableObject equivalent).

## Scripting

- **[Scripting](Scripting/Scripting.md)** — writing game logic in C# (and native C++): script types, lifecycle, the API, and hot reload.

## Packages

Index's extensibility model — add anything the core doesn't ship.

- **[Writing Packages](Packages/Writing-Packages.md)** — a practical guide to authoring your own package.
- **[Package System](Packages/Package-System.md)** — how packages are built, loaded, and distributed (the internals).

## Editor

- **[Editor](Editor/Editor.md)** — the visual tool for building scenes, testing in Play mode, and producing a finished game.

## Reference

- **[Third-Party Libraries](Reference/Third-Party-Libraries.md)** — the open-source libraries Index is built on.

---

## How the pieces fit together

```
        ┌─────────────────────────────────────────────┐
        │                   Editor                     │  ← you build the game here
        │   Hierarchy · Inspector · Viewport · Build   │
        └───────────────────────┬─────────────────────┘
                                 │ produces
                                 ▼
        Scenes ── contain ──► Entities ── have ──► Components
          │                                            ▲
          │ run                                        │ add your own via
          ▼                                            │
        Systems / Scripts (C#) ──────────────► Packages
          │
          │ heavy work →  Job System
          │ content    →  Assets
          ▼
        Runtime  ── boots everything in →  Startup Processes
```

- The **Editor** is where you author; the **Runtime** is what ships and runs your game (see [Startup Processes](Engine-Core/Startup-Processes.md)).
- A game is made of **Scenes**, which contain **Entities** built from **Components** ([ECS](Engine-Core/ECS.md)).
- **Scripts** ([Scripting](Scripting/Scripting.md)) and systems give entities behavior; **Packages** ([Writing Packages](Packages/Writing-Packages.md)) add new components and features.
- **Assets** ([Assets](Assets/Assets.md)) are the content; the **Job System** ([Job System](Engine-Core/Job-System.md)) parallelizes heavy work.
