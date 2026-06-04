# Startup Processes

This page explains **what happens, step by step, when you launch a game made with Index** — from double-clicking the `.exe` to the first frame your player sees.

If you just want the short version: the runtime boots the engine's subsystems in a fixed order, finds your project, loads your starting scene, runs your scripts, and then enters the main loop that draws every frame.

> This describes the **Runtime** (the shipped game), not the Editor. The Editor follows a very similar path but adds its tools on top.

---

## The big picture

A launched game goes through three stages:

1. **Boot** – start the engine and all its subsystems (window, graphics, audio, physics, …).
2. **Load** – find the project, load the startup scene, run packages and scripts.
3. **Run** – the main loop: read input, update the game, draw a frame, repeat — until the window closes.

```
Launch .exe
   │
   ▼
[1] Boot the engine ── window, graphics, audio, physics, jobs, assets …
   │
   ▼
[2] Load the game ──── project file → packages → startup scene → scripts
   │
   ▼
[3] Main loop ─────── input → update → physics → render → present  (every frame)
   │
   ▼
Window closed → shut everything down cleanly
```

---

## Stage 1 — Boot

### The entry point

Every Index program (game or editor) starts in the same place: a single `Main()` function (in `EntryPoint.hpp`). The very first things it does are:

1. **Initialize the core** – low-level setup (memory, assertions, logging).
2. **Initialize localization** – the language/translation system.
3. **Capture the command line** – arguments like `--project="C:/path"` are stored so the rest of the engine can read them.
4. **Create the application** – calls `CreateApplication()`, which builds the game's `Application` object.

If anything throws an error during the whole run, it is caught here and logged, so the game fails cleanly instead of crashing silently.

### Finding your project

Before the engine fully starts, the runtime decides **which project to run** (in `RuntimeApplication`):

- If you pass `--project="<path>"`, that folder is used.
- Otherwise it looks **next to the `.exe`** for a project.

It then reads your project's `index-project.json`. This file holds everything the game needs to know about itself: the window size, the splash screen, which scenes exist, and which scene to start with. If no project is found, the runtime logs a warning and falls back to a built-in sample scene so it still boots.

### Starting the subsystems

Next, `Application::Initialize()` starts every engine subsystem **in a fixed order**. Order matters — later systems depend on earlier ones (for example, the renderer needs the window and graphics device to already exist).

| # | Subsystem | What it does |
|---|-----------|--------------|
| 1 | **Profiler** | Performance measurement (only in profiling builds). |
| 2 | **Job System** | The worker-thread pool for multithreaded work. See [Job System](Job-System.md). |
| 3 | **Frame Arenas** | Fast per-frame scratch memory. |
| 4 | **Window** | Opens the OS window and input via **GLFW**. |
| 5 | **Render API** | Starts the graphics device — **WebGPU (via Dawn)**, which picks Direct3D 12 / Vulkan / Metal under the hood. |
| 6 | **Texture Manager** | Loads built-in textures, manages GPU textures. |
| 7 | **Shader Manager** | Loads and manages shaders. |
| 8 | **Renderer2D** | The 2D sprite/quad renderer. |
| 9 | **Gizmo Renderer** | Debug shapes/lines (optional in runtime). |
| 10 | **GUI Renderer** | The ImGui layer used for on-screen UI/overlays. |
| 11 | **Physics 2D** | The 2D physics world. |
| 12 | **Font Manager** | Loads and bakes fonts for text rendering. |
| 13 | **Asset Registry** | Registers built-in engine assets and indexes your project's assets. See [Assets](../Assets/Assets.md). |
| 14 | **Audio Manager** | Starts the audio device (via miniaudio). |

Some of these can be turned off through the application's configuration (for example, a headless server build may disable the window and audio).

---

## Stage 2 — Load

Once the subsystems are up, the runtime prepares your actual game.

### Configure scenes

The runtime calls `ConfigureScenes()`, which:

1. Puts the game into **play mode** immediately (a shipped game is always "playing").
2. Decides the **startup scene** using this priority:
   1. `StartupScene` from `index-project.json`, otherwise
   2. the last opened scene, otherwise
   3. a scene literally named `"SampleScene"`.
3. **Registers** every scene listed in the project's build scene list, and wires each one so that loading it reads the matching `.scene` file from disk.

At this point scenes are *registered* (the engine knows they exist and how to load them) but **not yet loaded**.

### Load packages

The runtime then calls `PackageHost::LoadAll()`. This scans next to the executable for package DLLs (`Pkg.<Name>.Native.dll`) and loads each one, calling its `IndexPackage_OnLoad()` function.

**Packages must load before any scene loads**, because a scene may contain components that a package defines. If the package isn't loaded first, the engine wouldn't know how to recreate those components. See [Writing Packages](../Packages/Writing-Packages.md).

### Splash screen (optional)

If your project enables a splash screen, the runtime pushes a **splash layer** here. When a splash is shown, the engine **defers loading the startup scene** until the splash finishes — so the player sees your logo first, and the (possibly heavy) scene load happens behind it. When the splash fades out, the deferred scene load runs.

### Load the startup scene

Whether immediately or after the splash, the engine loads the startup scene. Loading a scene means:

1. **Instantiate** the scene (create its systems and an empty entity world).
2. **Deserialize** the `.scene` file — recreate every saved entity and its components.
3. **Awake** all systems, then **Start** all systems.
4. Fire scene events (`ScenePreStartEvent` → `ScenePostStartEvent`) so other code can react.

See [Scenes](Scenes.md) for the full scene lifecycle.

### Start scripting

When the game needs C# code, the **Scripting host** boots: it starts the .NET runtime (via hostfxr), loads `Index-ScriptCore.dll` and your game's script assembly, and connects the native ↔ managed bridge. Then `OnApplicationStart` and the per-scene/per-entity script lifecycle begin running. See [Scripting](../Scripting/Scripting.md).

---

## Stage 3 — The main loop

Now the game is live. The engine runs the same loop every frame until the window is told to close. Each frame does this, in order:

```
┌────────────────────────────── one frame ──────────────────────────────┐
│ 1. Input    │ snapshot last frame's input, poll OS events, update state │
│ 2. Fixed    │ run FixedUpdate as many times as needed (physics-rate)    │
│    Update   │ — a fixed timestep, independent of frame rate             │
│ 3. Update   │ audio · global scripts · game logic · scene systems       │
│             │ · physics step · pre-render prep                          │
│ 4. Render   │ Renderer2D draws the scene · UI · gizmos                   │
│ 5. Present  │ swap the buffers — the finished frame appears on screen    │
└────────────────────────────────────────────────────────────────────────┘
```

A few details worth knowing:

- **Fixed Update** uses an accumulator. If the game runs fast, fixed update might run zero or one time per frame; if a frame was slow, it may run several times to "catch up." This keeps physics and gameplay stable regardless of frame rate.
- **Update** is where most game logic happens — your scripts' `OnUpdate`, scene systems, and so on.
- If a splash was shown, the deferred scene load is triggered early in this stage on the first eligible frame.
- The loop ends when you close the window (or the game calls quit).

---

## Stage 4 — Shutdown

When the loop ends, the engine tears everything down in a clean order: scenes are unloaded (systems get their `OnDestroy`), packages run `IndexPackage_OnUnload`, subsystems shut down, and the core shutdown guard runs last. This ensures files are flushed and resources are released properly.

---

## Quick reference: the boot order

```
Main()
 ├─ InitializeCore, Localization, parse command line
 ├─ CreateApplication()  → load index-project.json
 └─ Application::Run()
     ├─ Initialize()
     │   ├─ Profiler → JobSystem → FrameArenas
     │   ├─ Window (GLFW) → Render API (WebGPU/Dawn)
     │   ├─ Textures → Shaders → Renderer2D → Gizmos → GUI
     │   ├─ Physics2D → Fonts → Asset Registry → Audio
     │   ├─ ConfigureScenes()   (choose + register startup scene)
     │   ├─ ConfigureLayers()   (push splash if enabled)
     │   ├─ PackageHost::LoadAll()
     │   └─ Load startup scene (now, or after splash) → start scripts
     └─ Main loop:  input → fixed update → update → render → present
```

---

## Related pages

- [Scenes](Scenes.md) — how scenes load and what their lifecycle looks like.
- [ECS](ECS.md) — entities and components, the contents of a scene.
- [Assets](../Assets/Assets.md) — how textures, audio, fonts, etc. are found and loaded.
- [Scripting](../Scripting/Scripting.md) — the C# scripts that run during the loop.
- [Writing Packages](../Packages/Writing-Packages.md) — code loaded at the package step.
- [Job System](Job-System.md) — the worker threads started early in boot.
