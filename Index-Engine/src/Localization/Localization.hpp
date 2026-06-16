#pragma once
#include "Core/Export.hpp"

#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Index {

	namespace Localization {

		enum class LanguageStatus : std::uint8_t {
			Installed,        // file is locally available (bundled or user dir)
			Available,        // listed in manifest but not yet downloaded
			Downloading,
			DownloadFailed,
		};

		struct LanguageInfo {
			std::string Code;
			std::string DisplayName;
			LanguageStatus Status = LanguageStatus::Installed;
			bool RequiresCjkFont = false;
		};

		INDEX_API void Initialize();

		// Missing key: falls back to English, then logs once and returns a reference into a side map keyed by  so the raw identifier shows in the UI.
		INDEX_API const std::string& Get(std::string_view key);

		// English-fallback lookup, ignoring the active language. Use for UI shown
		// before a freshly-selected language's font is available (e.g. CJK), where
		// the localized text would render as missing-glyph boxes.
		INDEX_API const std::string& GetFallback(std::string_view key);

		INDEX_API const std::vector<LanguageInfo>& GetAvailableLanguages();
		INDEX_API const std::string& GetCurrentLanguage();

		// Returns the best installed match for the OS UI language (via GetUserDefaultLocaleName/$LANG), or "en" as fallback.
		INDEX_API std::string GetSystemLanguage();

		INDEX_API void SetLanguage(std::string_view code);

		// Kick off an async download for an `Available` language (and the
		// CJK font, if required and not already installed). No-op if a
		// download is already running or the language is not `Available`.
		INDEX_API void RequestLanguageDownload(std::string_view code);

		struct DownloadProgress {
			std::string Code;
			std::string Stage;
			float Progress = 0.0f;
			bool Running = false;
			bool Failed = false;
			bool RestartRequired = false;
			std::string Error;
		};

		// Snapshot of any in-flight or recently-finished download. UI polls
		// this each frame for progress display. Returns nullopt only when no
		// download has been initiated since Initialize().
		INDEX_API std::optional<DownloadProgress> GetActiveDownload();

		INDEX_API void Poll();

		using ChangeCallbackHandle = std::uint32_t;
		INDEX_API ChangeCallbackHandle RegisterChangeCallback(std::function<void()> cb);
		INDEX_API void UnregisterChangeCallback(ChangeCallbackHandle handle);

		// std::vformat-based positional substitution. Translation values use
		// {0}, {1} placeholders; missing-key fallback still applies because
		// the format string comes from Get(key).
		template <typename... Args>
		std::string Format(std::string_view key, Args&&... args) {
			const std::string& tmpl = Get(key);
			try {
				return std::vformat(tmpl, std::make_format_args(args...));
			}
			catch (const std::format_error&) {
				// Untrusted downloaded strings may have malformed format specs; degrade to raw template rather than throw.
				return tmpl;
			}
		}

		// Format() against the English fallback string (see GetFallback).
		template <typename... Args>
		std::string FormatFallback(std::string_view key, Args&&... args) {
			const std::string& tmpl = GetFallback(key);
			try {
				return std::vformat(tmpl, std::make_format_args(args...));
			}
			catch (const std::format_error&) {
				return tmpl;
			}
		}

	}

}

#define IDX_TR(key) ::Index::Localization::Get(key)
