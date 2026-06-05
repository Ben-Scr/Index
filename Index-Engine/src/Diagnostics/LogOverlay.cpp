// NOTE: excluded from the engine DLL compile set (see premake5.lua); Index-Editor and Index-Runtime each compile it directly.

#include "Diagnostics/LogOverlay.hpp"

#include <imgui.h>

#include <utility>

namespace Index::Diagnostics {

	namespace {
		// Color rows by severity — same palette ProfilerPanel / editor
		// console use so the overlay reads as "the same logs, smaller".
		ImVec4 ColorForLevel(Log::Level level) {
			switch (level) {
				case Log::Level::Trace:    return ImVec4(0.65f, 0.65f, 0.65f, 1.00f);
				case Log::Level::Info:     return ImVec4(0.92f, 0.92f, 0.92f, 1.00f);
				case Log::Level::Warn:     return ImVec4(1.00f, 0.85f, 0.30f, 1.00f);
				case Log::Level::Error:    return ImVec4(1.00f, 0.40f, 0.40f, 1.00f);
				case Log::Level::Critical: return ImVec4(1.00f, 0.20f, 0.20f, 1.00f);
			}
			return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		} 

		const char* ShortLevelTag(Log::Level level) {
			switch (level) {
				case Log::Level::Trace:    return "TRC";
				case Log::Level::Info:     return "INF";
				case Log::Level::Warn:     return "WRN";
				case Log::Level::Error:    return "ERR";
				case Log::Level::Critical: return "CRT";
			}
			return "???";
		}

		bool ShouldShow(Log::Type source) {
			return source == Log::Type::Client || source == Log::Type::EditorConsole;
		}
	}

	LogOverlay::LogOverlay() {
		m_SubscriptionId = Log::OnLog.Add([this](const Log::Entry& e) {
			OnLogEntry(e);
		});
	}

	LogOverlay::~LogOverlay() {
		Log::OnLog.Remove(m_SubscriptionId);
	}

	void LogOverlay::OnLogEntry(const Log::Entry& entry) {
		if (!ShouldShow(entry.Source)) return;

		std::scoped_lock lock(m_Mutex);
		m_Entries.push_back(StoredEntry{ entry.Message, entry.Level, entry.Source });
		while (m_Entries.size() > k_MaxEntries) {
			m_Entries.pop_front();
		}
	}

	void LogOverlay::ClearEntries() {
		std::scoped_lock lock(m_Mutex);
		m_Entries.clear();
	}

	void LogOverlay::RenderBody() const {
		ImGui::TextUnformatted("Logs");
		ImGui::Separator();

		// Snapshot under lock, render outside — rendering inside would block logging threads for the full ImGui draw.
		std::deque<StoredEntry> snapshot;
		{
			std::scoped_lock lock(m_Mutex);
			snapshot = m_Entries;
		}

		if (snapshot.empty()) {
			ImGui::TextDisabled("(no log entries yet)");
			return;
		}

		const float childWidth  = 420.0f;
		const float childHeight = 220.0f;
		ImGui::BeginChild("##IndexLogScroll", ImVec2(childWidth, childHeight),
			false, ImGuiWindowFlags_HorizontalScrollbar);

		const bool wasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
		for (const auto& e : snapshot) {
			const ImVec4 col = ColorForLevel(e.Level);
			ImGui::PushStyleColor(ImGuiCol_Text, col);
			// Same compact format used elsewhere: "[INF] message".
			ImGui::TextWrapped("[%s] %s", ShortLevelTag(e.Level), e.Message.c_str());
			ImGui::PopStyleColor();
		}
		// Auto-scroll only when we were already at the bottom — preserves
		// the user's manual scroll position when reading older entries.
		if (wasAtBottom) {
			ImGui::SetScrollHereY(1.0f);
		}
		ImGui::EndChild();
	}

	float LogOverlay::RenderInRect(const ImVec2& imageMin, const ImVec2& imageMax, float yOffset) const {
		constexpr float k_Pad = 8.0f;
		const ImVec2 overlayPivot(1.0f, 0.0f);
		const ImVec2 overlayPos(imageMax.x - k_Pad, imageMin.y + k_Pad + yOffset);

		ImGui::SetNextWindowPos(overlayPos, ImGuiCond_Always, overlayPivot);
		ImGui::SetNextWindowBgAlpha(0.78f);
		constexpr ImGuiWindowFlags k_OverlayFlags =
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoMove;

		float consumedHeight = 0.0f;
		if (ImGui::Begin("##IndexLogOverlayRect", nullptr, k_OverlayFlags)) {
			RenderBody();
			consumedHeight = ImGui::GetWindowSize().y;
		}
		ImGui::End();
		return consumedHeight;
	}

	float LogOverlay::RenderInMainViewport(float yOffset) const {
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		if (!vp) return 0.0f;
		constexpr float k_Pad = 10.0f;
		const ImVec2 overlayPivot(1.0f, 0.0f);
		const ImVec2 overlayPos(
			vp->WorkPos.x + vp->WorkSize.x - k_Pad,
			vp->WorkPos.y + k_Pad + yOffset);

		ImGui::SetNextWindowPos(overlayPos, ImGuiCond_Always, overlayPivot);
		ImGui::SetNextWindowBgAlpha(0.78f);
		constexpr ImGuiWindowFlags k_OverlayFlags =
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoMove;

		float consumedHeight = 0.0f;
		if (ImGui::Begin("##IndexLogOverlayViewport", nullptr, k_OverlayFlags)) {
			RenderBody();
			consumedHeight = ImGui::GetWindowSize().y;
		}
		ImGui::End();
		return consumedHeight;
	}

} // namespace Index::Diagnostics
