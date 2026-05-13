#pragma once

#include "Core/Base.hpp"
#include "Core/Export.hpp"
#include "Utils/Event.hpp"

#include <spdlog/fmt/fmt.h>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace Index {

	class INDEX_API Log {
	public:
		enum class Type : uint8_t {
			Core = 0,
			Client,
			EditorConsole
		};

		enum class Level : uint8_t {
			Trace = 0,
			Info,
			Warn,
			Error,
			Critical
		};

		struct Entry {
			std::string Message;
			Level Level = Level::Info;
			Type Source = Type::Core;
		};

		static void Initialize();
		static void Shutdown();
		static bool IsInitialized();

		static std::shared_ptr<spdlog::logger> GetCoreLogger();
		static std::shared_ptr<spdlog::logger> GetClientLogger();
		static std::shared_ptr<spdlog::logger> GetEditorConsoleLogger();

		static Event<const Entry&> OnLog;

		template <typename... Args>
		static void PrintMessage(const Type type, const Level level, fmt::format_string<Args...> format, Args&&... args) {
			if (!EnsureInitialized()) {
				return;
			}

			if (!ShouldLog(type, level)) {
				return;
			}

			const std::string message = fmt::format(format, std::forward<Args>(args)...);
			PrintMessage(type, level, std::string_view(message));
		}

		template <typename... Args>
		static void PrintMessageTag(const Type type, const Level level, std::string_view tag, fmt::format_string<Args...> format, Args&&... args) {
			if (!EnsureInitialized()) {
				return;
			}

			if (!ShouldLog(type, level)) {
				return;
			}

			const std::string message = fmt::format(format, std::forward<Args>(args)...);
			PrintMessageTag(type, level, tag, std::string_view(message));
		}

		static void PrintMessage(Type type, Level level, std::string_view message);
		static void PrintMessageTag(Type type, Level level, std::string_view tag, std::string_view message);

		static const char* LevelToString(Level level);

	private:
		static bool EnsureInitialized();
		static std::shared_ptr<spdlog::logger> SelectLogger(Type type);
		static spdlog::level::level_enum ToSpdlogLevel(Level level);
		static bool ShouldLog(Type type, Level level);
		static void Emit(std::shared_ptr<spdlog::logger>& logger, Level level, std::string_view message);

		// Defined out-of-line in Log.cpp so a single instance lives inside the
		// engine DLL. With inline-static in the header, every TU including
		// Log.hpp would see its own copy across the DLL boundary (the editor
		// EXE and the engine DLL would each have their own logger pointers).
		static std::mutex s_StateMutex;
		static bool s_Initialized;
		static std::shared_ptr<spdlog::logger> s_CoreLogger;
		static std::shared_ptr<spdlog::logger> s_ClientLogger;
		static std::shared_ptr<spdlog::logger> s_EditorConsoleLogger;
	};

} // namespace Index

#define IDX_CORE_TRACE(...) ::Index::Log::PrintMessage(::Index::Log::Type::Core, ::Index::Log::Level::Trace, __VA_ARGS__)
#define IDX_CORE_INFO(...) ::Index::Log::PrintMessage(::Index::Log::Type::Core, ::Index::Log::Level::Info, __VA_ARGS__)
#define IDX_CORE_WARN(...) ::Index::Log::PrintMessage(::Index::Log::Type::Core, ::Index::Log::Level::Warn, __VA_ARGS__)
#define IDX_CORE_ERROR(...) ::Index::Log::PrintMessage(::Index::Log::Type::Core, ::Index::Log::Level::Error, __VA_ARGS__)
#define IDX_CORE_FATAL(...) ::Index::Log::PrintMessage(::Index::Log::Type::Core, ::Index::Log::Level::Critical, __VA_ARGS__)

#define IDX_TRACE(...) ::Index::Log::PrintMessage(::Index::Log::Type::Client, ::Index::Log::Level::Trace, __VA_ARGS__)
#define IDX_INFO(...) ::Index::Log::PrintMessage(::Index::Log::Type::Client, ::Index::Log::Level::Info, __VA_ARGS__)
#define IDX_WARN(...) ::Index::Log::PrintMessage(::Index::Log::Type::Client, ::Index::Log::Level::Warn, __VA_ARGS__)
#define IDX_ERROR(...) ::Index::Log::PrintMessage(::Index::Log::Type::Client, ::Index::Log::Level::Error, __VA_ARGS__)
#define IDX_FATAL(...) ::Index::Log::PrintMessage(::Index::Log::Type::Client, ::Index::Log::Level::Critical, __VA_ARGS__)

#define IDX_CORE_TRACE_TAG(tag, ...) ::Index::Log::PrintMessageTag(::Index::Log::Type::Core, ::Index::Log::Level::Trace, tag, __VA_ARGS__)
#define IDX_CORE_INFO_TAG(tag, ...) ::Index::Log::PrintMessageTag(::Index::Log::Type::Core, ::Index::Log::Level::Info, tag, __VA_ARGS__)
#define IDX_CORE_WARN_TAG(tag, ...) ::Index::Log::PrintMessageTag(::Index::Log::Type::Core, ::Index::Log::Level::Warn, tag, __VA_ARGS__)
#define IDX_CORE_ERROR_TAG(tag, ...) ::Index::Log::PrintMessageTag(::Index::Log::Type::Core, ::Index::Log::Level::Error, tag, __VA_ARGS__)
#define IDX_CORE_FATAL_TAG(tag, ...) ::Index::Log::PrintMessageTag(::Index::Log::Type::Core, ::Index::Log::Level::Critical, tag, __VA_ARGS__)

#define IDX_TRACE_TAG(tag, ...) ::Index::Log::PrintMessageTag(::Index::Log::Type::Client, ::Index::Log::Level::Trace, tag, __VA_ARGS__)
#define IDX_INFO_TAG(tag, ...) ::Index::Log::PrintMessageTag(::Index::Log::Type::Client, ::Index::Log::Level::Info, tag, __VA_ARGS__)
#define IDX_WARN_TAG(tag, ...) ::Index::Log::PrintMessageTag(::Index::Log::Type::Client, ::Index::Log::Level::Warn, tag, __VA_ARGS__)
#define IDX_ERROR_TAG(tag, ...) ::Index::Log::PrintMessageTag(::Index::Log::Type::Client, ::Index::Log::Level::Error, tag, __VA_ARGS__)
#define IDX_FATAL_TAG(tag, ...) ::Index::Log::PrintMessageTag(::Index::Log::Type::Client, ::Index::Log::Level::Critical, tag, __VA_ARGS__)

#define IDX_CONSOLE_LOG_TRACE(...) ::Index::Log::PrintMessage(::Index::Log::Type::EditorConsole, ::Index::Log::Level::Trace, __VA_ARGS__)
#define IDX_CONSOLE_LOG_INFO(...) ::Index::Log::PrintMessage(::Index::Log::Type::EditorConsole, ::Index::Log::Level::Info, __VA_ARGS__)
#define IDX_CONSOLE_LOG_WARN(...) ::Index::Log::PrintMessage(::Index::Log::Type::EditorConsole, ::Index::Log::Level::Warn, __VA_ARGS__)
#define IDX_CONSOLE_LOG_ERROR(...) ::Index::Log::PrintMessage(::Index::Log::Type::EditorConsole, ::Index::Log::Level::Error, __VA_ARGS__)
#define IDX_CONSOLE_LOG_FATAL(...) ::Index::Log::PrintMessage(::Index::Log::Type::EditorConsole, ::Index::Log::Level::Critical, __VA_ARGS__)
