#pragma once

#include "Core/Export.hpp"
#include "Core/WindowSpecification.hpp"

#include <cstddef>

#ifndef INDEX_DEFAULT_ENABLE_WINDOW
#define INDEX_DEFAULT_ENABLE_WINDOW 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_GUI_RENDERER
#define INDEX_DEFAULT_ENABLE_GUI_RENDERER 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_GIZMO_RENDERER
#define INDEX_DEFAULT_ENABLE_GIZMO_RENDERER 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_PHYSICS_2D
#define INDEX_DEFAULT_ENABLE_PHYSICS_2D 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_AUDIO
#define INDEX_DEFAULT_ENABLE_AUDIO 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_SCRIPTING
#define INDEX_DEFAULT_ENABLE_SCRIPTING 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_RENDERER_2D
#define INDEX_DEFAULT_ENABLE_RENDERER_2D 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_TEXTURE_MANAGER
#define INDEX_DEFAULT_ENABLE_TEXTURE_MANAGER 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_SHADER_MANAGER
#define INDEX_DEFAULT_ENABLE_SHADER_MANAGER 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_PACKAGE_HOST
#define INDEX_DEFAULT_ENABLE_PACKAGE_HOST 1
#endif

#ifndef INDEX_DEFAULT_SET_WINDOW_ICON
#define INDEX_DEFAULT_SET_WINDOW_ICON 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_VSYNC
#define INDEX_DEFAULT_ENABLE_VSYNC 1
#endif

#ifndef INDEX_DEFAULT_FRAME_ARENA_CAPACITY_BYTES
#define INDEX_DEFAULT_FRAME_ARENA_CAPACITY_BYTES (1024 * 1024) // 1 MiB
#endif

#ifndef INDEX_DEFAULT_PERSISTENT_ARENA_CAPACITY_BYTES
#define INDEX_DEFAULT_PERSISTENT_ARENA_CAPACITY_BYTES (64 * 1024) // 64 KiB
#endif

namespace Index {

	struct ApplicationConfig {
		WindowSpecification WindowSpecification{ 800, 800, "Index Runtime", true, true, false };
		// These are runtime feature requests; build-time module availability is described by the
		// dependency sets in Dependencies.lua and the feature flags in Core/Export.hpp.
		// Off skips Window/RenderApi/cursor/icon creation entirely and runs
		// the engine with no GLFW window and no graphics backend. Subsystems
		// that need a render target (Renderer2D, GuiRenderer, GizmoRenderer,
		// Texture/Shader/Font managers) are force-disabled at Initialize
		// time even if their individual flags were left on — with a warning,
		// so the override isn't silent. The main loop exits on RequestQuit
		// alone (no WindowClose to listen for). Use this for AI/RL training,
		// headless servers, CI tests, batch asset processors. Pair with
		// ApplicationConfig::Headless() for sensible defaults.
		bool EnableWindow = INDEX_DEFAULT_ENABLE_WINDOW != 0;
		bool EnableGuiRenderer = INDEX_DEFAULT_ENABLE_GUI_RENDERER != 0;
		bool EnableGizmoRenderer = INDEX_DEFAULT_ENABLE_GIZMO_RENDERER != 0;
		bool EnablePhysics2D = INDEX_DEFAULT_ENABLE_PHYSICS_2D != 0;
		bool EnableAudio = INDEX_DEFAULT_ENABLE_AUDIO != 0;
		// Off skips the entire CoreCLR + ScriptCore + user-assembly setup.
		// The ScriptSystem is still added to scenes (cheap, no side effects),
		// but its Awake bails before loading anything. Use this for processes
		// that don't run game logic — the launcher is the obvious one: it
		// would otherwise hold a file lock on Index-ScriptCore.dll for its
		// whole lifetime, blocking any rebuild the editor tries to do.
		bool EnableScripting = INDEX_DEFAULT_ENABLE_SCRIPTING != 0;
		// Off skips constructing Renderer2D entirely. m_Renderer2D stays null
		// and BeginFrame / EndFrame already null-check it. Use this for
		// processes that don't render 2D content — the launcher draws only
		// ImGui via ImGuiContextLayer and never needs the batch renderer.
		bool EnableRenderer2D = INDEX_DEFAULT_ENABLE_RENDERER_2D != 0;
		// Off skips TextureManager::Initialize / Shutdown. Safe for the
		// launcher, which never loads sprites or thumbnails. The window-icon
		// path falls back to SetWindowIconFromResource (Win32 resource icon)
		// and only uses TextureManager when a project sets AppIconPath.
		bool EnableTextureManager = INDEX_DEFAULT_ENABLE_TEXTURE_MANAGER != 0;
		// Off skips ShaderManager::Initialize / Shutdown. Safe for any
		// process that doesn't load .wgsl shaders through the cache; the
		// renderers ship embedded WGSL via the WebGPU backend directly.
		bool EnableShaderManager = INDEX_DEFAULT_ENABLE_SHADER_MANAGER != 0;
		// Off skips PackageHost::LoadAll / UnloadAll. Native packages
		// (Pkg.<Name>.Native.dll next to the executable) are not discovered
		// or loaded. Use this for processes that don't run game packages —
		// the launcher has no project loaded so package OnLoad code would
		// just be dead-weight side effects.
		bool EnablePackageHost = INDEX_DEFAULT_ENABLE_PACKAGE_HOST != 0;
		bool SetWindowIcon = INDEX_DEFAULT_SET_WINDOW_ICON != 0;
		bool Vsync = INDEX_DEFAULT_ENABLE_VSYNC != 0;
		bool UseTargetFrameRateForMainLoop = true;

		// FrameArenas backing-buffer sizes. Frame() is reset every
		// EndFrame; Persistent() is never auto-reset. Set to 0 to opt
		// out — Allocate on a zero-capacity arena returns nullptr, so
		// callers that gracefully handle exhaustion still work.
		std::size_t FrameArenaCapacityBytes      = INDEX_DEFAULT_FRAME_ARENA_CAPACITY_BYTES;
		std::size_t PersistentArenaCapacityBytes = INDEX_DEFAULT_PERSISTENT_ARENA_CAPACITY_BYTES;

		// Worker-thread count for the engine JobSystem. -1 picks the
		// auto-resolved default (hardware_concurrency-1, capped to
		// cores-2 under INDEX_WITH_SCRIPTING, clamped to [2, 16]).
		// Positive values are passed through verbatim and clamped to
		// [1, 32]. Games can override via IDX_GAME's config struct;
		// scripts can also override at boot through JobSystem.Configure.
		int JobSystemWorkerCount = -1;

		// Minimal: window only, no rendering, no audio, no physics, no
		// scripting, no packages. For console tools that still want an
		// OS window (e.g. the launcher). EnableTextureManager stays on so
		// the window icon path still works.
		INDEX_API static ApplicationConfig Minimal();

		// Headless: no window, no graphics backend, no rendering. Keeps
		// scripting, physics, packages, audio. Disables the idle frame-cap
		// so the loop runs as fast as the simulation allows. For AI/RL
		// training, headless servers, CI tests, batch asset processors.
		INDEX_API static ApplicationConfig Headless();

		// Game runtime defaults: full rendering, physics, audio, scripting, packages.
		// Equivalent to today's default-constructed ApplicationConfig.
		INDEX_API static ApplicationConfig Game();

		// Editor-host defaults: same as Game() today; reserved as a future-proofing
		// hook so editor-only feature flags can be flipped here without touching
		// every caller.
		INDEX_API static ApplicationConfig Editor();
	};

} // namespace Index
