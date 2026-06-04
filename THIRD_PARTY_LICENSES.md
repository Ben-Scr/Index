# Third-Party Licenses

Index ships and/or statically links the following third-party software.
Each component's full license text is preserved in the corresponding
`External/<name>/LICENSE*` file in this repository; this document is the
aggregated attribution required by those licenses when Index is
redistributed in binary form.

The font bundle has its own attribution file:
[Index-Runtime/IndexAssets/Fonts/LICENSES.md](Index-Runtime/IndexAssets/Fonts/LICENSES.md).

## C++ libraries (statically linked into `Index-Engine.dll`)

| Component | License | Copyright | Source |
|---|---|---|---|
| Box2D | MIT | Copyright (c) 2022 Erin Catto | [External/box2d/LICENSE](External/box2d/LICENSE) |
| EnTT | MIT | Copyright (c) 2017-2026 Michele Caini | [External/entt/LICENSE](External/entt/LICENSE) |
| GLM | MIT / "Happy Bunny" | Copyright (c) 2005 G-Truc Creation | [External/glm/copying.txt](External/glm/copying.txt) |
| Dear ImGui | MIT | Copyright (c) 2014-2026 Omar Cornut | [External/imgui/LICENSE.txt](External/imgui/LICENSE.txt) |
| magic_enum | MIT | Copyright (c) 2019-2024 Daniil Goncharov | [External/magic_enum/LICENSE](External/magic_enum/LICENSE) |
| miniaudio | Public Domain / MIT-0 (choose) | David Reid | [External/miniaudio/LICENSE](External/miniaudio/LICENSE) |
| spdlog | MIT | Copyright (c) 2016-present Gabi Melman and spdlog contributors | [External/spdlog/LICENSE](External/spdlog/LICENSE) |
| stb | Public Domain / MIT (choose) | Copyright (c) 2017 Sean Barrett | [External/stb/LICENSE](External/stb/LICENSE) |
| Tracy (client) | 3-clause BSD | Copyright (c) 2017-2026 Bartosz Taudul | [External/tracy/LICENSE](External/tracy/LICENSE) |
| Dawn (WebGPU) | BSD 3-clause | Copyright 2017-2023 The Dawn & Tint Authors | [External/dawn/LICENSE](External/dawn/LICENSE) |
| concurrentqueue | BSD 2-clause / Boost Software License | Copyright (c) 2013-2016 Cameron Desrochers | [External/concurrentqueue/LICENSE.md](External/concurrentqueue/LICENSE.md) |
| glad | Apache 2.0 / MIT (generated from public Khronos OpenGL specs) | Copyright (c) 2013-2022 David Herberth | [External/glad](External/glad) |
| Index-Physics | Same license as Index (this engine) | Copyright (c) 2026 Ben Schneider | [External/Index-Physics](External/Index-Physics) |

### Dawn transitive dependencies

Dawn statically links several third-party components which are also
compiled into `Index-Engine.dll`. Their licenses (Apache 2.0, BSD,
MIT) ship with the Dawn source tree under
`External/dawn/third_party/<name>/LICENSE`. The most material ones are:

- **DirectX Shader Compiler / LLVM** — Apache 2.0 with LLVM exception.
- **Abseil** — Apache 2.0.
- **Protobuf** — BSD 3-clause.
- **glslang / SPIRV-Tools / SPIRV-Headers / SPIRV-Cross** — Apache 2.0 / MIT.
- **DirectX Headers** — MIT.

A copy of `External/dawn/third_party/` is **not** required for Index
binary redistribution as long as Dawn's own NOTICE/LICENSE files are
preserved in the engine's source distribution. Anyone redistributing
the engine's compiled DLL alongside this repository's source tree
satisfies the attribution requirement transitively.

## Shared libraries shipped alongside the executable

| Component | License | Source |
|---|---|---|
| GLFW (`GLFW.dll`) | zlib/libpng | [External/glfw/LICENSE.md](External/glfw/LICENSE.md) — Copyright (c) 2002-2006 Marcus Geelnard, Copyright (c) 2006-2019 Camilla Löwy |
| .NET host (`nethost.dll`) | MIT | The .NET Foundation. https://github.com/dotnet/runtime |
| Tracy (`Tracy.dll`) | 3-clause BSD | See Tracy entry above. Stripped from Dist builds when `--no-profiler` is passed. |

## Other code in this repository

| Path | License | Notes |
|---|---|---|
| `External/rapidxml/` | Boost-1.0 / MIT (dual) | Single vendored header (RapidXML 1.13, by Marcin Kalicinski). Used only by `CsprojParser.cpp` for .csproj/.props XML parsing. |
| `External/doctest/` | MIT | Test-only dependency. Not in shipped binaries. |
| `External/tracy/profiler/` | Various (3-clause BSD / MIT / Zlib / Apache 2.0 for transitive deps) | Tracy's standalone *viewer*. Not shipped with the engine; built separately by developers. |

## Fonts

See [Index-Runtime/IndexAssets/Fonts/LICENSES.md](Index-Runtime/IndexAssets/Fonts/LICENSES.md)
for the full per-font attribution.

## Engine

Index itself is licensed under the terms in [`LICENSE.txt`](LICENSE.txt).
