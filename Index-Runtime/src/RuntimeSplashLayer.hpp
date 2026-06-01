#pragma once

#include "Core/Layer.hpp"
#include "Graphics/TextureManager.hpp"

#include <string>

namespace Index {

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

		float m_FontColorR = 1.0f;
		float m_FontColorG = 1.0f;
		float m_FontColorB = 1.0f;
		float m_FontSize = 16.0f;

		std::string m_Subtitle;
		TextureHandle m_Logo;
		// Optional full-screen background image. Loaded from
		// project->SplashScreen.BackgroundImagePath the first frame
		// of the splash; same lifetime + reference-token rules as
		// m_Logo so the texture survives PurgeUnreferenced.
		TextureHandle m_Background;
		bool m_LogoLoadAttempted = false;
		// PurgeUnreferenced during startup-scene load would free m_Logo (not in any ECS); OnAttach registers a provider to keep it alive through the purge sweep.
		std::uint32_t m_TextureRefToken = 0;

		bool m_RequestPop = false;
	};

} // namespace Index
