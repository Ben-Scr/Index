#pragma once

#include "Graphics/Texture2D.hpp"

#include <string>

namespace Index { class Application; }

namespace Index::EditorRuntime {

	// Static-logo startup splash shared by Index-Launcher and Index-Editor.
	// Deliberately has NO start animation: the main thread blocks while startup
	// loading runs, so any intro tween would freeze mid-flight. The logo simply
	// holds (still) for as long as loading takes, then fades out once the host
	// reports completion. Draws into the ImGui foreground draw list, so it always
	// renders on top and needs an active ImGui frame (ImGuiContextLayer).
	class SplashScreen {
	public:
		// Loads the logo and marks the splash active (defers startup-scene load
		// via Application::SignalSplashAttached). Empty path → bundled icon.png.
		void Begin(const std::string& imagePath = {});

		bool IsActive() const { return m_Phase == Phase::Covering || m_Phase == Phase::FadeOut; }
		// True while fully opaque — the host should skip building its own UI.
		bool IsCovering() const { return m_Phase == Phase::Covering; }

		// One frame: draws the splash and advances the fade. Pass loadingComplete
		// = true to start the fade-out. Must run inside an ImGui frame.
		void Render(Application& app, bool loadingComplete);

		// Releases the GPU texture; call before WebGPU teardown.
		void Shutdown();

	private:
		enum class Phase { Idle, Covering, FadeOut, Done };

		void Draw(float alpha) const;

		Phase m_Phase = Phase::Idle;
		Texture2D m_Texture;
		float m_FadeTimer = 0.0f;
		int m_FramesShown = 0;
	};

}
