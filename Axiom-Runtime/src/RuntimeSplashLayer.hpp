#pragma once

#include "Core/Layer.hpp"
#include "Graphics/TextureManager.hpp"

#include <string>

namespace Axiom {

	// Runtime-only splash screen. Pushed by RuntimeApplication::Start() at the
	// very front of the layer stack when the project's SplashScreen.Enabled is
	// true (the default). Holds the screen for FadeIn + Duration + FadeOut
	// seconds, fading the user-supplied (or default Axiom) logo + a subtitle
	// line over a solid background, then pops itself off the stack.
	//
	// While the splash is up the layer eats input so the user can't click
	// through into not-yet-loaded UI; the log/stats overlays still draw on
	// top so dev builds keep their diagnostics. The runtime's scene update
	// keeps running normally — by the time the splash fades out the first
	// scene has already had a few frames to warm caches and run Awake/Start.
	class RuntimeSplashLayer : public Layer {
	public:
		using Layer::Layer;

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnUpdate(Application& app, float dt) override;
		void OnPreRender(Application& app) override;
		void OnPostRender(Application& app) override;

	private:
		bool m_ImGuiAcquired = false;

		float m_Elapsed = 0.0f;
		float m_FadeIn = 0.5f;
		float m_Hold = 1.5f;
		float m_FadeOut = 0.5f;

		float m_BackgroundR = 0.05f;
		float m_BackgroundG = 0.05f;
		float m_BackgroundB = 0.07f;

		std::string m_Subtitle;
		TextureHandle m_Logo;
		bool m_LogoLoadAttempted = false;

		bool m_RequestPop = false;
	};

} // namespace Axiom
