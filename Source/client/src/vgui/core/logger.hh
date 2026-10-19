#pragma once

/* @slog:
	tiny console logger the framework logs through.
	MSVC 14.29 (VS 2019 16.11) does NOT expose std::format_string with /std:c++20,
	so this port uses std::ostringstream + a tiny "{}" replacer instead of std::format.
*/

#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <windows.h>

namespace slog
{
	namespace detail
	{
		enum class severity : int {
			info = 0,
			success,
			warning,
			error,
			debug
		};

		inline bool g_enabled{ true };

		// Optional external sink. VantaClient's bootstrap registers one that
		// pipes into vanta.log, so vgui traces (font loading, overlay create,
		// device init, ...) become visible in the same file as everything else.
		using external_sink_t = void(*)(int level, const char* message);
		inline external_sink_t g_external_sink{ nullptr };

		template <class T>
		inline std::string stringify(const T& v) {
			if constexpr (std::is_convertible_v<T, std::string_view>) return std::string(std::string_view(v));
			else if constexpr (std::is_arithmetic_v<T>)               return std::to_string(v);
			else { std::ostringstream os; os << v; return os.str(); }
		}

		/* @vformat:
			substitutes "{}" and, as far as the argument is already a string,
			"{:spec}" too. The spec itself is IGNORED, not honoured — the point
			is that a call site written for std::format still prints its value
			instead of leaking a literal "0x{:x}" into the log, which is exactly
			what it used to do.
		*/
		inline std::string vformat(std::string_view fmt, std::initializer_list<std::string> args) {
			std::string out;
			out.reserve(fmt.size() + args.size() * 8);
			auto it = args.begin();
			for (std::size_t i = 0; i < fmt.size(); ) {
				if (i + 1 < fmt.size() && fmt[i] == '{' && fmt[i+1] == '{') { out.push_back('{'); i += 2; continue; }
				if (i + 1 < fmt.size() && fmt[i] == '}' && fmt[i+1] == '}') { out.push_back('}'); i += 2; continue; }

				if (fmt[i] == '{') {
					// scan to the matching '}' — "{}" or "{:...}" both land here
					const std::size_t close = fmt.find('}', i + 1);
					const bool simple = close != std::string_view::npos
						&& (close == i + 1 || fmt[i + 1] == ':');
					if (simple) {
						if (it != args.end()) { out.append(*it); ++it; }
						i = close + 1;
						continue;
					}
				}

				out.push_back(fmt[i++]);
			}
			return out;
		}

		inline void emit(severity level, const std::string& message)
		{
#ifndef _DEBUG
			(void)level;
			(void)message;
#else
			if (!g_enabled) return;
			const char* tag = "[ ]"; WORD attribute = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
			switch (level) {
			case severity::info:    tag = "[*]"; attribute = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
			case severity::success: tag = "[+]"; attribute = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
			case severity::warning: tag = "[!]"; attribute = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
			case severity::error:   tag = "[-]"; attribute = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
			case severity::debug:   tag = "[~]"; attribute = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
			}
			const HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
			CONSOLE_SCREEN_BUFFER_INFO previous{};
			const bool has_console = console != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(console, &previous) != FALSE;
			if (has_console) SetConsoleTextAttribute(console, attribute);
			std::printf("%s %s\n", tag, message.c_str());
			if (has_console) SetConsoleTextAttribute(console, previous.wAttributes);
			OutputDebugStringA((std::string(tag) + " " + message + "\n").c_str());
			if (g_external_sink) g_external_sink(static_cast<int>(level), message.c_str());
#endif
		}
	}

	namespace log
	{
		inline void set_enabled(bool enabled) { detail::g_enabled = enabled; }
		inline void set_external_sink(detail::external_sink_t sink) { detail::g_external_sink = sink; }

		template <typename... A>
		inline void info(std::string_view fmt, const A&... args) {
			detail::emit(detail::severity::info,    detail::vformat(fmt, {detail::stringify(args)...}));
		}
		template <typename... A>
		inline void success(std::string_view fmt, const A&... args) {
			detail::emit(detail::severity::success, detail::vformat(fmt, {detail::stringify(args)...}));
		}
		template <typename... A>
		inline void warning(std::string_view fmt, const A&... args) {
			detail::emit(detail::severity::warning, detail::vformat(fmt, {detail::stringify(args)...}));
		}
		template <typename... A>
		inline void error(std::string_view fmt, const A&... args) {
			detail::emit(detail::severity::error,   detail::vformat(fmt, {detail::stringify(args)...}));
		}
		template <typename... A>
		inline void debug(std::string_view fmt, const A&... args) {
			detail::emit(detail::severity::debug,   detail::vformat(fmt, {detail::stringify(args)...}));
		}
		inline void info   (std::string_view msg) { detail::emit(detail::severity::info,    std::string(msg)); }
		inline void success(std::string_view msg) { detail::emit(detail::severity::success, std::string(msg)); }
		inline void warning(std::string_view msg) { detail::emit(detail::severity::warning, std::string(msg)); }
		inline void error  (std::string_view msg) { detail::emit(detail::severity::error,   std::string(msg)); }
		inline void debug  (std::string_view msg) { detail::emit(detail::severity::debug,   std::string(msg)); }
	}
}
