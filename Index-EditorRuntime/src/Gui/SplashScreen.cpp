#include "Gui/SplashScreen.hpp"

#include "Core/Application.hpp"
#include "Core/Time.hpp"
#include "Graphics/Filter.hpp"
#include "Graphics/Wrap.hpp"
#include "Serialization/Path.hpp"

#include <algorithm>
#include <imgui.h>

namespace Index::EditorRuntime {

	namespace {
		constexpr float k_FadeSeconds = 0.40f;
		// Logo fits within this fraction of the shorter viewport edge.
		constexpr float k_LogoFraction = 0.40f;
	}

	void SplashScreen::Begin(const std::string& imagePath) {
		std::string path = imagePath;
		if (path.empty()) {
			const std::string dir = Path::ResolveIndexAssets("Textures");
			if (!dir.empty()) {
				path = Path::Combine(dir, "icon.png");
			}
		}

		if (!path.empty()) {
			// flip=false + straight UVs: logo draws upright.
			m_Texture = Texture2D(path.c_str(), Filter::Bilinear, Wrap::Clamp, Wrap::Clamp,
				/*mipmaps*/true, /*srgb*/false, /*flip*/false);
		}

		if (m_Texture.IsValid()) {
			m_Phase = Phase::Covering;
			m_FadeTimer = 0.0f;
			m_FramesShown = 0;
			// Defers Application's startup-scene load until the splash completes.
			Application::SignalSplashAttached();
		}
		else {
			m_Phase = Phase::Done;
		}
	}

	void SplashScreen::Render(Application& app, bool loadingComplete) {
		if (m_Phase == Phase::Idle || m_Phase == Phase::Done) {
			return;
		}

		if (m_Phase == Phase::Covering) {
			Draw(1.0f);
			++m_FramesShown;
			// Begin the fade only once loading is done AND the splash has actually
			// been presented (≥2 frames), so a fast load still shows the logo.
			if (loadingComplete && m_FramesShown >= 2) {
				m_Phase = Phase::FadeOut;
				m_FadeTimer = 0.0f;
			}
			return;
		}

		// FadeOut — unscaled dt so Time.timeScale can't affect the splash.
		m_FadeTimer += app.GetTime().GetDeltaTimeUnscaled();
		const float alpha = 1.0f - (k_FadeSeconds > 0.0f ? m_FadeTimer / k_FadeSeconds : 1.0f);
		if (alpha <= 0.0f) {
			m_Phase = Phase::Done;
			Shutdown();
			// Releases the deferred startup-scene gate (no-op if already loaded).
			Application::SignalSplashDetached();
			return;
		}
		Draw(alpha);
	}

	void SplashScreen::Draw(float alpha) const {
		alpha = std::clamp(alpha, 0.0f, 1.0f);

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImDrawList* dl = ImGui::GetForegroundDrawList();

		const ImVec2 pMin = vp->Pos;
		const ImVec2 pMax(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
		// Themed opaque background that fades together with the logo.
		dl->AddRectFilled(pMin, pMax, ImGui::GetColorU32(ImGuiCol_WindowBg, alpha));

		if (!m_Texture.IsValid()) {
			return;
		}
		const float texW = m_Texture.GetWidth();
		const float texH = m_Texture.GetHeight();
		if (texW <= 0.0f || texH <= 0.0f) {
			return;
		}

		const float fit = std::min(vp->Size.x * k_LogoFraction / texW,
			vp->Size.y * k_LogoFraction / texH);
		const float drawW = texW * fit;
		const float drawH = texH * fit;
		const ImVec2 center(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);
		const ImVec2 a(center.x - drawW * 0.5f, center.y - drawH * 0.5f);
		const ImVec2 b(a.x + drawW, a.y + drawH);

		const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f + 0.5f));
		dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(m_Texture.GetHandle())),
			a, b, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
	}

	void SplashScreen::Shutdown() {
		m_Texture.Destroy();
	}

}
