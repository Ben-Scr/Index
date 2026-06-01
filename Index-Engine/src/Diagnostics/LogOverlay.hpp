#pragma once

#include "Collections/Ids.hpp"
#include "Core/Log.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

// NOT marked INDEX_API: each consumer (Editor, Runtime) compiles the .cpp into its own binary; no DLL boundary.
struct ImVec2;

namespace Index::Diagnostics {

	class LogOverlay {
	public:
		// Cap on retained entries — older ones are dropped FIFO when the
		// buffer fills. 256 is enough to scroll back through a couple of
		// seconds of busy logging without unbounded memory growth.
		static constexpr std::size_t k_MaxEntries = 256;

		LogOverlay();
		~LogOverlay();

		LogOverlay(const LogOverlay&) = delete;
		LogOverlay& operator=(const LogOverlay&) = delete;
		LogOverlay(LogOverlay&&) = delete;
		LogOverlay& operator=(LogOverlay&&) = delete;

		// Render entry points mirror StatsOverlay. Both return the height
		// the rendered window actually consumed so callers can stack
		// further overlays below if needed.
		float RenderInRect(const ImVec2& imageMin, const ImVec2& imageMax, float yOffset = 0.0f) const;
		float RenderInMainViewport(float yOffset = 0.0f) const;

		// Drop every retained entry. Called by the inspector "Clear"
		// button if/when one ships; useful from scripts too.
		void ClearEntries();

	private:
		void OnLogEntry(const Log::Entry& entry);
		void RenderBody() const;

		struct StoredEntry {
			std::string Message;
			Log::Level Level = Log::Level::Info;
			Log::Type Source = Log::Type::Client;
		};

		mutable std::mutex m_Mutex;
		std::deque<StoredEntry> m_Entries;
		EventId m_SubscriptionId{};
	};

} // namespace Index::Diagnostics
