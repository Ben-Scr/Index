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
		// Off: no GLFW window or graphics backend; render-dependent subsystems force-disabled at Initialize (with warning). Use Headless() for sensible defaults.
		bool EnableWindow = INDEX_DEFAULT_ENABLE_WINDOW != 0;
		bool EnableGuiRenderer = INDEX_DEFAULT_ENABLE_GUI_RENDERER != 0;
		bool EnableGizmoRenderer = INDEX_DEFAULT_ENABLE_GIZMO_RENDERER != 0;
		bool EnablePhysics2D = INDEX_DEFAULT_ENABLE_PHYSICS_2D != 0;
		bool EnableAudio = INDEX_DEFAULT_ENABLE_AUDIO != 0;
		// Off: skips CoreCLR + ScriptCore + user-assembly setup; avoids holding a file lock on Index-ScriptCore.dll (blocks editor rebuilds).
		bool EnableScripting = INDEX_DEFAULT_ENABLE_SCRIPTING != 0;
		bool EnableRenderer2D = INDEX_DEFAULT_ENABLE_RENDERER_2D != 0;
		bool EnableTextureManager = INDEX_DEFAULT_ENABLE_TEXTURE_MANAGER != 0;
		bool EnableShaderManager = INDEX_DEFAULT_ENABLE_SHADER_MANAGER != 0;
		bool EnablePackageHost = INDEX_DEFAULT_ENABLE_PACKAGE_HOST != 0;
		bool SetWindowIcon = INDEX_DEFAULT_SET_WINDOW_ICON != 0;
		bool Vsync = INDEX_DEFAULT_ENABLE_VSYNC != 0;
		bool UseTargetFrameRateForMainLoop = true;

		// F11 toggles true exclusive fullscreen (glfwSetWindowMonitor). Games
		// want it; the editor and launcher do NOT — exclusive fullscreen takes
		// over the display and breaks their multi-window UI (secondary OS
		// windows get covered / auto-iconified). Those apps set this false.
		bool EnableFullscreenToggle = true;

		std::size_t FrameArenaCapacityBytes      = INDEX_DEFAULT_FRAME_ARENA_CAPACITY_BYTES;
		std::size_t PersistentArenaCapacityBytes = INDEX_DEFAULT_PERSISTENT_ARENA_CAPACITY_BYTES;

		// -1 = auto (hw_concurrency-1, capped to cores-2 under INDEX_WITH_SCRIPTING, clamped [2,16]); positive values clamped to [1,32].
		int JobSystemWorkerCount = -1;

		// Window only — no rendering/audio/physics/scripting/packages. EnableTextureManager stays on so the window icon path works.
		INDEX_API static ApplicationConfig Minimal();

		INDEX_API static ApplicationConfig Headless();

		// Game runtime defaults: full rendering, physics, audio, scripting, packages.
		// Equivalent to today's default-constructed ApplicationConfig.
		INDEX_API static ApplicationConfig Game();

		INDEX_API static ApplicationConfig Editor();
	};

} // namespace Index
