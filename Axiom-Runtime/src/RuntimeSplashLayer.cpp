#include "RuntimeSplashLayer.hpp"
#include "RuntimeImGuiHost.hpp"

#include "Collections/Viewport.hpp"
#include "Core/Application.hpp"
#include "Core/Time.hpp"
#include "Core/Version.hpp"
#include "Core/Window.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Project/AxiomProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/Path.hpp"

#include <algorithm>
#include <filesystem>
#include <imgui.h>
#include <string>

namespace Axiom {

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

		std::string DefaultBuildLine() {
			std::string profile;
#if defined(AXIOM_BUILD_RELEASE)
			profile = "Release";
#elif defined(AXIOM_BUILD_DEVELOPMENT)
			profile = "Development";
#else
			profile = "Development";
#endif
			std::string platform;
#if defined(AIM_PLATFORM_WINDOWS)
			platform = "Windows";
#elif defined(__APPLE__)
			platform = "macOS";
#else
			platform = "Linux";
#endif
			// Latin-1 supplement covers '·' (U+00B7) — the engine's text
			// renderer atlas now bakes 32-126 + 160-255. ImGui's default
			// font here renders the Latin-1 set since codepoints 160-255
			// fall inside its baked range too. Higher codepoints like
			// '•' (U+2022) still need the SDF/on-demand path.
			return std::string("Axiom ") + AIM_VERSION + "  ·  " + platform + "  ·  " + profile;
		}

		std::string ResolveDefaultLogoPath() {
			std::string root = Path::ResolveAxiomAssets("Textures");
			if (root.empty()) return {};
			std::filesystem::path candidate = std::filesystem::path(root) / "Axiom64.png";
			if (std::filesystem::exists(candidate)) return candidate.string();
			candidate = std::filesystem::path(root) / "Axiom16.png";
			if (std::filesystem::exists(candidate)) return candidate.string();
			candidate = std::filesystem::path(root) / "icon.png";
			if (std::filesystem::exists(candidate)) return candidate.string();
			return {};
		}

		std::string ResolveCustomLogoPath(const std::string& projectRelative) {
			if (projectRelative.empty()) return {};
			AxiomProject* project = ProjectManager::GetCurrentProject();
			if (!project) return projectRelative;

			std::filesystem::path absolute(projectRelative);
			if (absolute.is_absolute() && std::filesystem::exists(absolute)) {
				return absolute.string();
			}
			// AppIconPath / SplashScreen.ImagePath are stored relative to the
			// project root (Assets/foo.png style). Try that first, then fall
			// back to the cwd next to the executable.
			std::filesystem::path projectRel = std::filesystem::path(project->RootDirectory) / projectRelative;
			if (std::filesystem::exists(projectRel)) return projectRel.string();
			return projectRelative;
		}
	}

	void RuntimeSplashLayer::OnAttach(Application& app) {
		m_ImGuiAcquired = RuntimeImGuiHost::Acquire(app.GetWindow());

		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (project) {
			m_FadeIn = std::max(0.0f, project->SplashScreen.FadeInSeconds);
			m_Hold = std::max(0.0f, project->SplashScreen.DurationSeconds);
			m_FadeOut = std::max(0.0f, project->SplashScreen.FadeOutSeconds);
			m_BackgroundR = project->SplashScreen.BackgroundR;
			m_BackgroundG = project->SplashScreen.BackgroundG;
			m_BackgroundB = project->SplashScreen.BackgroundB;
			m_Subtitle = project->SplashScreen.CustomText.empty()
				? DefaultBuildLine() : project->SplashScreen.CustomText;
		}
		else {
			m_Subtitle = DefaultBuildLine();
		}
	}

	void RuntimeSplashLayer::OnDetach(Application&) {
		if (m_ImGuiAcquired) {
			RuntimeImGuiHost::Release();
			m_ImGuiAcquired = false;
		}
	}

	void RuntimeSplashLayer::OnUpdate(Application& app, float /*dt*/) {
		if (m_RequestPop) return;
		// Use unscaled dt — splash timing should not respond to Time.timeScale.
		m_Elapsed += app.GetTime().GetDeltaTimeUnscaled();

		const float total = m_FadeIn + m_Hold + m_FadeOut;
		if (m_Elapsed >= total) {
			m_RequestPop = true;
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
			AxiomProject* project = ProjectManager::GetCurrentProject();
			if (project && !project->SplashScreen.ImagePath.empty()) {
				logoPath = ResolveCustomLogoPath(project->SplashScreen.ImagePath);
			}
			if (logoPath.empty()) {
				logoPath = ResolveDefaultLogoPath();
			}
			if (!logoPath.empty()) {
				m_Logo = TextureManager::LoadTexture(logoPath);
			}
		}

		RuntimeImGuiHost::BeginFrame();

		int width = 0, height = 0;
		if (Window* window = app.GetWindow()) {
			if (Viewport* vp = Window::GetMainViewport()) {
				width = vp->GetWidth();
				height = vp->GetHeight();
			}
			else {
				width = window->GetWidth();
				height = window->GetHeight();
			}
		}
		if (width <= 0 || height <= 0) return;

		// Fade timeline. Alpha curves linearly from 0→1 over FadeIn,
		// stays 1 during Hold, then 1→0 over FadeOut. The background
		// fades on top so the previous frame doesn't bleed through.
		float alpha = 1.0f;
		if (m_Elapsed < m_FadeIn && m_FadeIn > 0.0f) {
			alpha = m_Elapsed / m_FadeIn;
		}
		else if (m_Elapsed > m_FadeIn + m_Hold && m_FadeOut > 0.0f) {
			alpha = 1.0f - (m_Elapsed - m_FadeIn - m_Hold) / m_FadeOut;
		}
		alpha = std::clamp(alpha, 0.0f, 1.0f);

		ImGui::SetNextWindowPos(ImVec2(0, 0));
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
		ImGui::Begin("##AxiomSplash", nullptr, flags);

		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 wMin = ImGui::GetWindowPos();
		const ImVec2 wMax = ImVec2(wMin.x + width, wMin.y + height);
		draw->AddRectFilled(wMin, wMax, PackColor(m_BackgroundR, m_BackgroundG, m_BackgroundB, alpha));

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
					ImVec2(0, 1), ImVec2(1, 0),
					tint);
			}
		}

		// Subtitle line — engine version + platform + build profile, OR
		// the project's customText override.
		if (!m_Subtitle.empty()) {
			const ImVec2 textSize = ImGui::CalcTextSize(m_Subtitle.c_str());
			const ImVec2 textPos(centerX - textSize.x * 0.5f,
				centerY + std::min(width, height) * 0.18f);
			draw->AddText(textPos, PackColor(1.0f, 1.0f, 1.0f, alpha * 0.85f), m_Subtitle.c_str());
		}

		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
	}

	void RuntimeSplashLayer::OnPostRender(Application&) {
		if (!m_ImGuiAcquired || !RuntimeImGuiHost::IsInitialized()) return;
		RuntimeImGuiHost::EndFrame();
	}

} // namespace Axiom
