#pragma once
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Zero-dep logging. No std::format (MSVC 14.29 in VS 2019 does not expose
// std::format_string with /std:c++20). Uses a tiny "{}" placeholder replacer
// with std::ostringstream stringification per arg.

namespace vanta::log {

enum class Level : int { Trace = 0, Debug, Info, Warn, Error, Fatal };

void init(Level minLevel, std::filesystem::path logFile = {});
void setLevel(Level minLevel);
void write(Level lvl, std::string_view scope, std::string msg) noexcept;
void shutdown() noexcept;

namespace detail {

template <class T>
inline std::string toStr(const T& v) {
    if constexpr (std::is_convertible_v<T, std::string_view>) {
        return std::string(std::string_view(v));
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(v);
    } else {
        std::ostringstream os; os << v; return os.str();
    }
}

inline std::string vformat(std::string_view fmt, std::initializer_list<std::string> args) {
    std::string out;
    out.reserve(fmt.size() + args.size() * 8);
    auto it = args.begin();
    for (std::size_t i = 0; i < fmt.size(); ) {
        if (i + 1 < fmt.size() && fmt[i] == '{' && fmt[i+1] == '}') {
            if (it != args.end()) { out.append(*it); ++it; }
            i += 2;
        } else if (i + 1 < fmt.size() && fmt[i] == '{' && fmt[i+1] == '{') {
            out.push_back('{'); i += 2;
        } else if (i + 1 < fmt.size() && fmt[i] == '}' && fmt[i+1] == '}') {
            out.push_back('}'); i += 2;
        } else {
            out.push_back(fmt[i++]);
        }
    }
    return out;
}

} // namespace detail

template <class... Args>
inline void writef(Level lvl, std::string_view scope, std::string_view fmt, const Args&... args) noexcept {
    try {
        write(lvl, scope, detail::vformat(fmt, {detail::toStr(args)...}));
    } catch (...) {
        write(lvl, scope, "<log format error>");
    }
}

// Zero-arg overload avoids the initializer_list-of-empty-braces path.
inline void writef(Level lvl, std::string_view scope, std::string_view msg) noexcept {
    write(lvl, scope, std::string(msg));
}

} // namespace vanta::log

#ifdef _DEBUG
#define VLOG_TRACE(scope, ...) ::vanta::log::writef(::vanta::log::Level::Trace, scope, __VA_ARGS__)
#define VLOG_DEBUG(scope, ...) ::vanta::log::writef(::vanta::log::Level::Debug, scope, __VA_ARGS__)
#define VLOG_INFO(scope, ...)  ::vanta::log::writef(::vanta::log::Level::Info,  scope, __VA_ARGS__)
#define VLOG_WARN(scope, ...)  ::vanta::log::writef(::vanta::log::Level::Warn,  scope, __VA_ARGS__)
#define VLOG_ERR(scope, ...)   ::vanta::log::writef(::vanta::log::Level::Error, scope, __VA_ARGS__)
#define VLOG_FATAL(scope, ...) ::vanta::log::writef(::vanta::log::Level::Fatal, scope, __VA_ARGS__)
#else
// Release's sink is disabled; do not allocate/format its discarded messages.
#define VLOG_TRACE(...) ((void)0)
#define VLOG_DEBUG(...) ((void)0)
#define VLOG_INFO(...)  ((void)0)
#define VLOG_WARN(...)  ((void)0)
#define VLOG_ERR(...)   ((void)0)
#define VLOG_FATAL(...) ((void)0)
#endif
