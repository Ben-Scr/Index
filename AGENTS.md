# Index Engine

2D game engine in C++20 with C# scripting. WebGPU rendering (via Dawn), EnTT ECS, ImGui editor, Box2D physics, miniaudio audio.

## Build

- **Build system:** Premake5 generates VS2022 solutions (Windows) or gmake2 (Linux).
- **Setup:** `Setup.bat` or `python scripts/Setup.py` — inits submodules, runs premake.
- **Premake:** `./vendor/bin/premake5.exe vs2022`
- **Build (CLI):** `"C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" Index.sln -p:Configuration=Debug -p:Platform=x64 -verbosity:minimal`
- **Startup project:** Index-Launcher
- **Configs:** Debug, Release, Dist. All x64.
- **Output:** `bin/{config}-windows-x64/{project}/`
- **C++ Standard:** C++20 (Box2D and Index-Physics use C++23)
- **New package:** `python scripts/packages/NewPackage.py <PackageName>` scaffolds a package under `packages/<PackageName>/` with a starter `index-package.lua`.

## Projects

| Project | Type | Purpose |
|---------|------|---------|
| Index-Engine | StaticLib | Core engine — everything else links this |
| Index-Editor | ConsoleApp | Scene editor (single .cpp, uses ImGuiEditorSystem) |
| Index-Runtime | ConsoleApp | Runs built games (single .cpp) |
| Index-Launcher | ConsoleApp | Project launcher UI (single .cpp) |
| Index-ScriptCore | C# SharedLib | Managed scripting API (.NET 9.0) |
| Index-Sandbox | C# SharedLib | Example game project |

## Code conventions

- **Namespace:** `Index` for everything. Sub-namespaces rare.
- **Member variables:** `m_PascalCase` (private), `s_PascalCase` (static), `k_PascalCase` (constants). Public fields are `PascalCase` without prefix.
- **Methods:** PascalCase. Getters: `Get*`, `Is*`, `Has*`. Setters: `Set*`.
- **Templates:** `T`, `TComponent`, `TTag`, `TSystem`.
- **Headers:** `#pragma once`. Include order: pch, local engine, third-party, std.
- **Logging:** Use `IDX_CORE_*_TAG(tag, fmt, args...)` for engine internals, `IDX_*_TAG(tag, fmt, args...)` for client-facing. Uses spdlog fmt syntax, NOT string concatenation.
- **Assertions:** `IDX_ASSERT(cond, IndexErrorCode::X, "message")`, `IDX_CORE_ASSERT(...)`.
- **Components:** Value-type structs, no inheritance. Register in `BuiltInComponentRegistration.cpp`.
- **Tags:** Empty structs (e.g. `DisabledTag`), use `requires std::is_empty_v<TTag>`.
- **Systems:** Inherit `ISystem`, implement lifecycle: `Awake`, `Start`, `Update`, `FixedUpdate`, `OnGui`, `OnDestroy`.
- **PCH:** `pch.hpp`. Files that conflict with PCH opt out via premake filter (e.g. glad.c, scripting files).
- **Export:** `INDEX_API` macro for DLL-visible symbols.

## Key patterns

- Handle-based resources: `TextureHandle` (index+generation), `AudioHandle` (uint32 ID).
- Scene lifecycle: `SceneDefinition` (template) -> `Scene` (instance with registry + systems).
- Component access: `scene.GetComponent<T>(entity)`, `entity.GetComponent<T>()`.
- Events: `EventDispatcher(event).Dispatch<EventType>(handler)` and `Event<Args...>` pub/sub.
- Serialization: Hand-written JSON (no library). `SceneSerializer::SaveToFile/LoadFromFile`.
- Project config: `index-project.json` with hand-rolled JSON parsing.

## Known issues / tech debt

- `Debugging/Logger.hpp/.cpp` is dead code (superseded by `Core/Log`). Can be deleted.
- `IndexProject.cpp` has hand-rolled JSON parsing — verbose, but `Json::Value::FindMember` null-guards on non-object so degraded inputs are handled. Migrating to typed accessors would still cut LOC.
- `SoundRequest::GetHandle` is a data member named like an accessor.
- `Scene::CreateDetachedScene` / `Scene::IsDetached` (renamed from `CreateDetachedEditorScene` / `IsEditorPreview`) are still in the core surface — kept engine-neutral so the editor *uses* them but the engine doesn't *know* about the editor.
- Editor-only Application state (`m_IsGameplayPaused`, `m_IsScriptInputEnabled`, `m_EditorStopPlayRequested`) is mutated through `ApplicationEditorAccess` (Index-Editor side, friend class). The single exception is `Application::RequestEditorStopPlay()`, which Core exposes for the engine-side script bindings (`ScriptBindingsScene.cpp`); it's a no-op outside the editor.
- Texture / audio lifecycle is managed via `TextureManager::PurgeUnreferenced` and `AudioManager::PurgeUnreferenced`, called from `SceneManager::LoadScene` / `UnloadAllScenes` (SceneManager.cpp:362, 485). The bare `UnloadTexture` / `UnloadAudio` entry points are not the primary cleanup path; non-ECS holders opt in via the `ReferenceProvider` hook.
- `RapidXML` (vendored single header at `External/rapidxml/`) is used only by `CsprojParser.cpp` for .csproj/.props XML parsing.

## File structure (engine)

```
Index-Engine/src/
  Core/         Application, Window, Input, Time, Log, Memory, Assert, UUID
  Graphics/     Renderer2D, Texture2D, TextureManager, Shader, GizmoRenderer, OpenGL
  Scene/        Scene, SceneManager, SceneDefinition, Entity, ComponentRegistry, ISystem
  Components/   Transform2D, Camera2D, SpriteRenderer, Rigidbody2D, BoxCollider2D, AudioSource, ParticleSystem2D, Tags
  Physics/      PhysicsSystem2D, Box2DWorld, Physics2D (static query API)
  Audio/        AudioManager, Audio, AudioHandle
  Scripting/    DotNetHost, ScriptEngine, ScriptBindings, ScriptSystem (C# only — native code lives in packages)
  Serialization/ SceneSerializer, File, Path, Directory, FileWatcher
  Project/      IndexProject, ProjectManager, LauncherRegistry
  Gui/          GuiRenderer (editor panels live in Index-Editor/, not here)
  Systems/      AudioUpdateSystem, GizmosDebugSystem, ParticleUpdateSystem
  Events/       IndexEvent, KeyEvents, MouseEvents, WindowEvents, SceneEvents
  Math/         VectorMath, Trigonometry, Common, Random
  Collections/  Vec2, Vec4, Color, AABB, Viewport, Ids
  Utils/        Event, Timer, StringHelper, CommandBuffer
  Debugging/    Logger (dead code)
```

## External dependencies (in External/; git submodules except the vendored RapidXML header)

GLFW, Glad, ImGui (docking), EnTT, GLM, Box2D, Index-Physics, spdlog, miniaudio, STB, magic_enum, RapidXML (vendored single header), .NET SDK, Tracy (profiler client + viewer)

## Profiling

Two complementary layers, both fed from the same `INDEX_PROFILE_*` macros in `Index-Engine/src/Profiling/Profiler.hpp`:

- **In-editor `ProfilerPanel`** — lightweight at-a-glance dashboard. Per-module rolling avg/min/max + scrolling line graph. Drives the "Sampling Hz" / "Tracking Span" knobs; gated by panel visibility unless "Track in Background" is on.
- **Tracy** — full timeline / flamegraph / per-thread lane / memory / plots. The Tracy client is compiled with `TRACY_ON_DEMAND` ([premake/dependencies/tracy.lua](premake/dependencies/tracy.lua)) — zero cost until a viewer attaches.

### Connecting Tracy

1. Build the standalone viewer once: `External/tracy/profiler/build/win32/Tracy.sln` (VS2022, x64 Release) or `cmake -B build -DCMAKE_BUILD_TYPE=Release` under `External/tracy/profiler/`. Drop the resulting `Tracy.exe` somewhere stable (e.g. `tools/Tracy.exe`, gitignored).
2. Build Index in **Release** (Dist strips the profiler entirely; Debug works but is slow).
3. Launch the editor / runtime — Tracy client is silent until a viewer attaches.
4. Launch `Tracy.exe`. On localhost it auto-discovers via UDP broadcast on port 8086. First Windows Firewall prompt: allow. If discovery fails, connect manually to `127.0.0.1`.

### Macros

- `INDEX_PROFILE_SCOPE(name)` — RAII zone. Tracy zone + in-engine ring-buffer push (if `name` matches a registered module).
- `INDEX_PROFILE_FRAME(name)` — call once per frame to advance sampling cadence and emit Tracy frame mark.
- `INDEX_PROFILE_VALUE(name, val)` — Tracy plot + value push (counts, MB, etc.).
- `INDEX_PROFILE_THREAD_NAME(name)` — call once at thread entry so the lane is labeled in Tracy.

All four become `(void)0` when the profiler is stripped (`--no-profiler` at premake time).

### What Tracy sees today

- **CPU zones**: 52+ instrumentation sites across `Application`, `Renderer2D`, `JobSystem`, scene update, UI, particles, scripting. Physics / scene-graph internals / asset IO are still dark — adding `INDEX_PROFILE_SCOPE` sites is cheap.
- **Threads with semantic names**: `Main`, `Index Worker 0..N` (cross-platform), `FileWatcher`, `LanguageDownloader`, `ScriptProcess`. Other threads show as bare TIDs.
- **Memory allocations**: only on Windows Debug builds with `IDX_TRACK_MEMORY` — `Index-Engine/src/Core/Memory.cpp` hooks `TracyAlloc`/`TracyFree` from inside the gated `operator new/delete`.
- **GPU**: surfaces as a Tracy plot (`GPU.ms`) — no dedicated GPU lane yet (no `TracyWebGPU` integration). The in-engine `GpuTimer` resolves WebGPU timestamp queries through a 3-deep readback ring.
