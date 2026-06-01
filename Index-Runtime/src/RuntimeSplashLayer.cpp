#include "RuntimeSplashLayer.hpp"
#include "RuntimeImGuiHost.hpp"

#include "Collections/Color.hpp"
#include "Collections/Viewport.hpp"
#include "Core/Application.hpp"
#include "Core/Log.hpp"
#include "Core/Time.hpp"
#include "Core/Window.hpp"
#include "Graphics/RenderApi.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Project/SplashAssetResolve.hpp"

#include <algorithm>
#include <imgui.h>
#include <string>

namespace Index {

	namespace {
		// Solid 0xAARRGGBB encoded for ImGui draw list.
		ImU32 PackColor(float r, float g, float b, float a) {
			r = std::clamp(r, 0.0f, 1.0f);
			g = std::clamp(g, 0.0f, 1.0f);
			b = std::clamp(b, 0.0f, 1.0f);
			a = std::clamp(a, 0.0f, 1.0f);
			return IM_COL32(
				static_cast<int>(r * 255.0f),
				static_cast<int>(g * 255.0f),
				static_cast<int>(b * 255.0f),
				static_cast<int>(a * 255.0f));
		}
	}

	void RuntimeSplashLayer::OnAttach(Application& app) {
		m_ImGuiAcquired = RuntimeImGuiHost::Acquire(app.GetWindow());

		// MUST precede scene load: without this signal Application::Init loads scenes synchronously, rendering the splash over an already-loaded scene.
		Application::SignalSplashAttached();

		m_TextureRefToken = TextureManager::AddReferenceProvider(
			[this](const TextureManager::ReferenceEmitter& emit) {
				if (m_Logo.IsValid()) emit(m_Logo);
				if (m_Background.IsValid()) emit(m_Background);
			});

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (project) {
			m_FadeIn = std::max(0.0f, project->SplashScreen.FadeInSeconds);
			m_Hold = std::max(0.0f, project->SplashScreen.DurationSeconds);
			m_FadeOut = std::max(0.0f, project->SplashScreen.FadeOutSeconds);
			m_BackgroundR = project->SplashScreen.BackgroundR;
			m_BackgroundG = project->SplashScreen.BackgroundG;
			m_BackgroundB = project->SplashScreen.BackgroundB;
			m_FontColorR = project->SplashScreen.FontColorR;
			m_FontColorG = project->SplashScreen.FontColorG;
			m_FontColorB = project->SplashScreen.FontColorB;
			m_FontSize = project->SplashScreen.FontSize;
			m_Subtitle = project->SplashScreen.CustomText.empty()
				? SplashAssetResolve::DefaultSubtitleLine() : project->SplashScreen.CustomText;
		}
		else {
			m_Subtitle = SplashAssetResolve::DefaultSubtitleLine();
		}
	}

	void RuntimeSplashLayer::OnDetach(Application&) {
		if (m_TextureRefToken != 0) {
			TextureManager::RemoveReferenceProvider(m_TextureRefToken);
			m_TextureRefToken = 0;
		}
		if (m_ImGuiAcquired) {
			RuntimeImGuiHost::Release();
			m_ImGuiAcquired = false;
		}

		// MUST be last: teardown above completes before the scene-load stutter.
		Application::SignalSplashDetached();
	}

	void RuntimeSplashLayer::OnUpdate(Application& app, float /*dt*/) {
		if (m_RequestPop) return;
		// Use unscaled dt — splash timing should not respond to Time.timeScale.
		m_Elapsed += app.GetTime().GetDeltaTimeUnscaled();

		const float total = m_FadeIn + m_Hold + m_FadeOut;
		if (m_Elapsed >= total) {
			m_RequestPop = true;
			// PopOverlay defers the erase but runs OnDetach immediately, firing SignalSplashDetached so the next BeginFrame loads the startup scene.
			app.PopOverlay(this);
		}
	}

	void RuntimeSplashLayer::OnPreRender(Application& app) {
		if (!m_ImGuiAcquired || !RuntimeImGuiHost::IsInitialized()) return;
		if (m_RequestPop) return;

		// Lazy-load the logo on the first render frame so OpenGL context
		// is guaranteed to exist (TextureManager::LoadTexture creates a GL
		// texture on first call).
		if (!m_LogoLoadAttempted) {
			m_LogoLoadAttempted = true;
			std::string logoPath;
			bool customRequested = false;
			IndexProject* project = ProjectManager::GetCurrentProject();
			if (project && !project->SplashScreen.ImagePath.empty()) {
				customRequested = true;
				logoPath = SplashAssetResolve::Resolve(project->SplashScreen.ImagePath, project);
			}
			if (logoPath.empty()) {
				logoPath = SplashAssetResolve::DefaultLogoPath();
			}
			if (!logoPath.empty()) {
				m_Logo = TextureManager::LoadTexture(logoPath);
			}
			// Log exactly once with the resolved path; without this the only diagnostic was an OutOfRange index from GetTexture, pointing at the wrong site.
			if (customRequested && !TextureManager::IsValid(m_Logo)) {
				IDX_CORE_WARN_TAG("RuntimeSplash",
					"Custom splash image '{}' failed to load (resolved: '{}'); falling back to no logo.",
					project->SplashScreen.ImagePath,
					logoPath.empty() ? std::string("<unresolved>") : logoPath);
			}

			if (project && !project->SplashScreen.BackgroundImagePath.empty()) {
				const std::string bgPath = SplashAssetResolve::Resolve(
					project->SplashScreen.BackgroundImagePath, project);
				if (!bgPath.empty()) {
					m_Background = TextureManager::LoadTexture(bgPath);
				}
				if (!TextureManager::IsValid(m_Background)) {
					IDX_CORE_WARN_TAG("RuntimeSplash",
						"Splash background image '{}' failed to load (resolved: '{}'); using solid colour fallback.",
						project->SplashScreen.BackgroundImagePath,
						bgPath.empty() ? std::string("<unresolved>") : bgPath);
				}
			}
		}

		// Aspect-locked: ImGui manages its own viewport so the cached-viewport path doesn't apply; clear the full swap chain first, then offset the ImGui window to the sub-rect.
		int width = 0, height = 0;
		int offsetX = 0, offsetY = 0;
		bool hasLetterbox = false;
		if (Window* window = app.GetWindow()) {
			if (Viewport* vp = Window::GetMainViewport()) {
				width = vp->GetWidth();
				height = vp->GetHeight();
				offsetX = vp->GetOffsetX();
				offsetY = vp->GetOffsetY();
				hasLetterbox = vp->HasLetterbox();
			}
			else {
				width = window->GetWidth();
				height = window->GetHeight();
			}
		}
		if (width <= 0 || height <= 0) return;

		if (hasLetterbox && RenderApi::IsInitialized()) {
			RenderApi::BindDefaultFramebuffer();
			RenderApi::SetClearColor(Color{ 0.0f, 0.0f, 0.0f, 1.0f });
			RenderApi::Clear(ClearFlags::Color | ClearFlags::Depth);
		}

		RuntimeImGuiHost::BeginFrame();

		float alpha = 1.0f;
		if (m_Elapsed < m_FadeIn && m_FadeIn > 0.0f) {
			alpha = m_Elapsed / m_FadeIn;
		}
		else if (m_Elapsed > m_FadeIn + m_Hold && m_FadeOut > 0.0f) {
			alpha = 1.0f - (m_Elapsed - m_FadeIn - m_Hold) / m_FadeOut;
		}
		alpha = std::clamp(alpha, 0.0f, 1.0f);

		ImGui::SetNextWindowPos(ImVec2(static_cast<float>(offsetX), static_cast<float>(offsetY)));
		ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width), static_cast<float>(height)));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
		const ImGuiWindowFlags flags =
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
			ImGuiWindowFlags_NoFocusOnAppearing;
		ImGui::Begin("##IndexSplash", nullptr, flags);

		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 wMin = ImGui::GetWindowPos();
		const ImVec2 wMax = ImVec2(wMin.x + width, wMin.y + height);
		// Solid fill first as fallback/underlay; background image drawn cover-style (shorter side fills, centred) on top.
		draw->AddRectFilled(wMin, wMax, PackColor(m_BackgroundR, m_BackgroundG, m_BackgroundB, alpha));

		Texture2D* background = TextureManager::GetTexture(m_Background);
		if (background && background->IsValid()) {
			const float bgW = static_cast<float>(background->GetWidth());
			const float bgH = static_cast<float>(background->GetHeight());
			if (bgW > 0.0f && bgH > 0.0f) {
				const float canvasAspect = static_cast<float>(width) / static_cast<float>(height);
				const float bgAspect = bgW / bgH;
				float drawW = static_cast<float>(width);
				float drawH = static_cast<float>(height);
				if (bgAspect > canvasAspect) {
					// Image is wider than canvas — fit to height, overflow horizontally.
					drawW = drawH * bgAspect;
				}
				else {
					// Image is taller — fit to width, overflow vertically.
					drawH = drawW / bgAspect;
				}
				const ImVec2 bgMin(wMin.x + (width  - drawW) * 0.5f,
				                   wMin.y + (height - drawH) * 0.5f);
				const ImVec2 bgMax(bgMin.x + drawW, bgMin.y + drawH);
				const ImU32 bgTint = PackColor(1.0f, 1.0f, 1.0f, alpha);
				draw->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(background->GetHandle())),
					bgMin, bgMax,
					ImVec2(0, 0), ImVec2(1, 1),
					bgTint);
			}
		}

		const float centerX = wMin.x + width * 0.5f;
		const float centerY = wMin.y + height * 0.5f;

		Texture2D* logo = TextureManager::GetTexture(m_Logo);
		if (logo && logo->IsValid()) {
			const float maxLogoSide = std::min(width, height) * 0.35f;
			float logoW = static_cast<float>(logo->GetWidth());
			float logoH = static_cast<float>(logo->GetHeight());
			if (logoW > 0 && logoH > 0) {
				const float scale = std::min(maxLogoSide / logoW, maxLogoSide / logoH);
				logoW *= scale;
				logoH *= scale;
				const ImVec2 imgMin(centerX - logoW * 0.5f, centerY - logoH * 0.65f);
				const ImVec2 imgMax(imgMin.x + logoW, imgMin.y + logoH);
				const ImU32 tint = PackColor(1.0f, 1.0f, 1.0f, alpha);
				draw->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(logo->GetHandle())),
					imgMin, imgMax,
					ImVec2(0, 0), ImVec2(1, 1),
					tint);
			}
		}

		if (!m_Subtitle.empty()) {
			ImFont* font = ImGui::GetFont();
			const float fontSize = (m_FontSize > 0.0f) ? m_FontSize : ImGui::GetFontSize();
			const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, m_Subtitle.c_str());
			const ImVec2 textPos(centerX - textSize.x * 0.5f,
				centerY + std::min(width, height) * 0.18f);
			draw->AddText(font, fontSize, textPos,
				PackColor(m_FontColorR, m_FontColorG, m_FontColorB, alpha * 0.85f),
				m_Subtitle.c_str());
		}

		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
	}

	void RuntimeSplashLayer::OnPostRender(Application&) {
		if (!m_ImGuiAcquired || !RuntimeImGuiHost::IsInitialized()) return;
		RuntimeImGuiHost::EndFrame();
	}

} // namespace Index
