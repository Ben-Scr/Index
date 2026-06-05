# Editor

The **Index Editor** is the visual tool where you build your game: arrange entities, edit their components, design scenes, test in Play mode, and build a finished game. This page is a tour of the editor and its core workflows.

If you just want the short version: the editor is a separate app that opens your project. You build scenes in the **Hierarchy** and **Viewport**, edit selected objects in the **Inspector**, manage files in the **Asset Browser**, and press **Play** to test.

---

## Editor, engine, and runtime

It helps to know how the pieces relate:

- The **Editor** (`Index-Editor`) is an authoring tool. It embeds the engine and adds visual tools on top. It does *not* ship with your game.
- The **Runtime** (`Index-Runtime`) is the slim executable that *runs* your finished game using the scenes and assets you authored. See [Startup Processes](../Engine-Core/Startup-Processes.md).
- The **Launcher** (`Index-Launcher`) is the small app you use to **create** and **open** projects.

So the flow is: **Launcher → create/open a project → Editor → build your game → Runtime runs it.**

---

## Opening a project

You create and open projects from the **Launcher**:

- **New Project** — pick a name and location; the launcher copies a template, sets up the C# script project, and creates an empty starting scene.
- **Open** — pick an existing project; the launcher prepares it and opens the editor.

A project is a folder with an `project.json` (its settings: scenes, window size, packages, build options) plus folders for your `Assets`, scripts, and scenes.

---

## The panels

The editor is a set of dockable panels. The main ones:

| Panel | What it's for |
|-------|---------------|
| **Hierarchy** (Entities) | The tree of all entities in the loaded scene(s). Create, rename, parent, reorder, and delete entities here. |
| **Inspector** | Shows and edits the components of whatever you've selected. This is where most editing happens. |
| **Editor View** (Viewport) | The interactive 2D canvas. Move the camera, select objects, and drag gizmos to position them. |
| **Game View** | A preview of the game exactly as the player will see it (active during Play mode). |
| **Asset Browser** | Your project's files — sprites, scenes, prefabs, scripts. Create assets, drag them into scenes, slice sprites. See [Assets](../Assets/Assets.md). |
| **Console** | Engine and script log output (info, warnings, errors). |
| **Profiler** | Real-time performance timings per subsystem. |
| **Package Manager** | Browse and install [packages](../Packages/Writing-Packages.md). |
| **Project Settings** | Display, graphics, branding (icon/splash), build, and system settings. |
| **Build** | Configure and produce a runnable game. |

A **toolbar** at the top has the playback controls (Play / Pause / Step / Stop), and a **menu bar** has Save, project tools, preferences, and more.

---

## Core workflows

### Selecting and editing an entity

1. Click an entity in the **Hierarchy** (or click it directly in the **Viewport**).
2. The **Inspector** shows its components.
3. Edit values — move it, change its sprite, tweak script fields, toggle it on/off.

The scene is marked **unsaved** (a dot/asterisk) as soon as you change something.

### Adding components

In the Inspector, click **Add Component**. A searchable list appears with every available component — built-in ones *and* anything your [packages](../Packages/Writing-Packages.md) added. Components that would conflict with what the entity already has are blocked, so you can't create invalid combinations.

### Building the hierarchy

In the **Hierarchy** panel:

- **Right-click** empty space to create entities — empty objects, sprites, physics bodies, UI elements, particles, cameras, and so on.
- **Drag** one entity onto another to make it a **child** (it keeps its on-screen position).
- **Drag** to reorder siblings.
- Select and press **Delete** to remove an entity and its children.

Children follow their parent's transform — see [ECS](../Engine-Core/ECS.md#hierarchy-parents-and-children).

### Working in the Viewport

The **Editor View** is your 2D workspace:

- **Pan** with the middle mouse button, **zoom** with the scroll wheel.
- **Focus** the camera on the selected entity (frames it in view).
- **Click** a sprite to select it; hold **Ctrl/Shift** to multi-select.
- With **Gizmos** enabled, drag the on-screen handles to **move, rotate, and scale** the selected entity.

There are also display toggles (wireframe vs. filled, post-processing on/off) to help you see what you're doing.

### Play, Pause, Stop

The toolbar controls let you test the game inside the editor:

- **Play** — saves your scenes, (re)loads them from disk, and starts the game. Scripts and systems begin running. The **Game View** shows the result.
- **Pause** — freezes the game so you can inspect it; the editor stays responsive.
- **Step** — while paused, advance a single frame (great for debugging).
- **Stop** — ends play and **reloads the scene from disk**, discarding anything that changed during play.

That last point is important: changes you make to entities *while playing* are intentionally thrown away when you stop. See [Scenes › Play mode](../Engine-Core/Scenes.md#play-mode-in-the-editor).

### Saving

Save the current scene from the menu (or with the usual save shortcut). Scenes are written to `.scene` files in your project. The editor can also **auto-save** at an interval you set in preferences.

---

## Prefabs

A **prefab** is a reusable, saved entity. In the editor you can:

- **Create** a prefab from an entity (it becomes a `.prefab` asset).
- **Edit** a prefab in a dedicated mode that opens just that entity tree.
- **Instantiate** it by dragging the `.prefab` into a scene — each copy is an *instance* that can have its own overrides.

Saving changes to a prefab updates every instance (except the parts an instance has overridden). See [Scenes › Prefabs](../Engine-Core/Scenes.md#prefabs).

---

## Building your game

When you're ready to ship, open the **Build** panel:

1. Choose a **build profile** (target platform and render backend).
2. Set the **scene list** — which scenes ship, and which one is the **startup** scene.
3. Pick an **output folder** and click **Build** (or **Build & Run**).

The editor compiles your C# scripts, copies the **runtime executable**, copies your **assets**, and writes the configuration the game needs to start. The result is a folder you can run or distribute. Because Index ships [assets as loose files](../Assets/Assets.md#assets-at-runtime-vs-in-the-editor), the output contains your `Assets` alongside the executable.

Build profiles can be managed separately (create/duplicate/delete) so you can keep, say, a Windows debug profile and a release profile side by side.

---

## Summary

- The **Editor** authors your game; the **Runtime** runs it; the **Launcher** creates/opens projects.
- Build scenes with the **Hierarchy** and **Viewport**, edit with the **Inspector**, manage files in the **Asset Browser**.
- **Add Component** respects conflicts; the **Viewport** gizmos move/rotate/scale entities.
- **Play/Pause/Step/Stop** test the game in-editor; stopping restores the scene from disk.
- **Prefabs** are reusable entities; the **Build** panel produces a distributable game.

## Related pages

- [ECS](../Engine-Core/ECS.md) — entities and components you edit.
- [Scenes](../Engine-Core/Scenes.md) — scenes, prefabs, and Play mode.
- [Assets](../Assets/Assets.md) — the Asset Browser and how assets are stored.
- [Scripting](../Scripting/Scripting.md) — the code behind your entities.
- [Writing Packages](../Packages/Writing-Packages.md) — extend the editor with new components.
