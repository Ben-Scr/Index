# Third-Party Libraries

Index is built on a small set of well-established open-source libraries. This page lists what they are and what each one does in the engine, so you know where a given capability comes from.

Index's philosophy is to keep mandatory dependencies few and focused — each library below earns its place by doing one job well.

---

## Rendering & windowing

| Library | Role in Index |
|---------|---------------|
| [WebGPU via Dawn](https://dawn.googlesource.com/dawn) | The graphics backend. Dawn implements the WebGPU API and **picks the right native backend at runtime** — Direct3D 12, Vulkan, or Metal — depending on the platform. |
| [GLFW](https://github.com/glfw/glfw) | Opens the OS window and handles keyboard/mouse input. |
| [Dear ImGui](https://github.com/ocornut/imgui) | The immediate-mode UI used to build the editor's panels and the runtime's on-screen overlays. |

---

## Simulation & gameplay

| Library | Role in Index |
|---------|---------------|
| [EnTT](https://github.com/skypjack/entt) | The Entity Component System — how entities and components are stored and iterated. See [ECS](../Engine-Core/ECS.md). |
| [Box2D](https://github.com/erincatto/box2d) / [Index-Physics](https://github.com/Ben-Scr/Index-Physics) | 2D physics — rigid bodies, colliders, and collision detection. |

---

## Audio & assets

| Library | Role in Index |
|---------|---------------|
| [miniaudio](https://github.com/mackron/miniaudio) | The audio backend — device output and sound playback. |
| [GLM](https://github.com/g-truc/glm) | Vector and matrix math. |
| [STB](https://github.com/nothings/stb) | Image loading (decoding PNG/JPG/etc. into textures). |

---

## Scripting

| Library | Role in Index |
|---------|---------------|
| [.NET 9 / hostfxr](https://dotnet.microsoft.com/) | Hosts the C# scripting runtime. The engine boots .NET through hostfxr and loads the scripting assemblies. See [Scripting](../Scripting/Scripting.md). |

---

## A note on supported frameworks

For C# scripting, Index targets **.NET 9.0**. Scripts and C# packages build against that runtime.

## Related pages

- [Startup Processes](../Engine-Core/Startup-Processes.md) — where these libraries are initialized during boot.
- [Scripting](../Scripting/Scripting.md) — the .NET scripting host.
- [ECS](../Engine-Core/ECS.md) — how EnTT is used.
