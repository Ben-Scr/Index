#include "pch.hpp"
#include "Core/ApplicationConfig.hpp"

namespace Index {

	ApplicationConfig ApplicationConfig::Minimal() {
		ApplicationConfig config;
		config.EnableGuiRenderer = false;
		config.EnableGizmoRenderer = false;
		config.EnablePhysics2D = false;
		config.EnableAudio = false;
		config.EnableScripting = false;
		config.EnableRenderer2D = false;
		// EnableTextureManager stays on so the window-icon path can still load
		// a project icon (a one-shot upload, not a per-frame cost).
		config.EnableTextureManager = true;
		// No rendering in Minimal -> no shaders to cache.
		config.EnableShaderManager = false;
		config.EnablePackageHost = false;
		// Window-icon and vsync are not subsystem toggles — leave them alone.
		return config;
	}

	ApplicationConfig ApplicationConfig::Headless() {
		ApplicationConfig config;
		config.EnableWindow = false;
		// Subsystems below all need a window / GL context. Application::Initialize
		// also force-disables these with a warning when EnableWindow=false, so
		// users who flip EnableWindow manually still get a working build — but
		// we set them here too so the factory's intent is self-documenting.
		config.EnableGuiRenderer = false;
		config.EnableGizmoRenderer = false;
		config.EnableRenderer2D = false;
		config.EnableTextureManager = false;
		config.EnableShaderManager = false;
		config.SetWindowIcon = false;
		config.Vsync = false;
		// Run as fast as the simulation allows — RL training and asset
		// processing typically want unbounded throughput. Callers that
		// want a cap can flip this back on and set SetTargetFramerate.
		config.UseTargetFrameRateForMainLoop = false;
		// Kept on intentionally: EnablePhysics2D, EnableAudio, EnableScripting,
		// EnablePackageHost. Game logic / training environments usually need them.
		return config;
	}

	ApplicationConfig ApplicationConfig::Game() {
		// Today's default-constructed config — full game runtime: rendering,
		// physics, audio, scripting, packages. Lives here so callers can spell
		// the intent (`ApplicationConfig::Game()`) instead of relying on the
		// implicit default.
		return ApplicationConfig{};
	}

	ApplicationConfig ApplicationConfig::Editor() {
		// Editor host has no extra flags today, but the factory exists so that
		// flipping editor-only defaults later is a one-line change.
		return Game();
	}

} // namespace Index
