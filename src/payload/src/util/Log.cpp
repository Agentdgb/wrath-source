#include "util/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <windows.h>

namespace vanta::log {

namespace {

constexpr const char* kLevelTag[] = {"TRC", "DBG", "INF", "WRN", "ERR", "FTL"};
constexpr WORD        kLevelColor[] = {
    FOREGROUND_INTENSITY,                                                      // TRC
    FOREGROUND_GREEN | FOREGROUND_BLUE,                                        // DBG
    FOREGROUND_GREEN | FOREGROUND_INTENSITY,                                   // INF
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,                  // WRN
    FOREGROUND_RED | FOREGROUND_INTENSITY,                                     // ERR
    FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,                   // FTL
};

std::atomic<int>  g_minLevel{static_cast<int>(Level::Info)};
std::mutex        g_sinkMx;
std::FILE*        g_sink   = nullptr;
HANDLE            g_stderr = nullptr;
std::atomic<bool> g_initDone{false};

const char* baseName(const char*) noexcept { return ""; }

} // namespace

void init(Level minLevel, std::filesystem::path logFile) {
#ifndef _DEBUG
    (void)minLevel;
    (void)logFile;
#else
    std::scoped_lock lk(g_sinkMx);
    g_minLevel.store(static_cast<int>(minLevel), std::memory_order_relaxed);

    if (!g_stderr) g_stderr = GetStdHandle(STD_ERROR_HANDLE);

    if (!logFile.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(logFile.parent_path(), ec);
        if (g_sink) { std::fclose(g_sink); g_sink = nullptr; }
        g_sink = _wfopen(logFile.wstring().c_str(), L"ab");
        if (!g_sink) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "[vanta] log file open FAILED (errno=%d): %ls\n",
                errno, logFile.wstring().c_str());
            OutputDebugStringA(buf);
        } else {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "[vanta] log opened at %ls\n", logFile.wstring().c_str());
            OutputDebugStringA(buf);
        }
    }
    g_initDone.store(true, std::memory_order_release);
#endif
}

void setLevel(Level minLevel) {
    g_minLevel.store(static_cast<int>(minLevel), std::memory_order_relaxed);
}

void write(Level lvl, std::string_view scope, std::string msg) noexcept {
#ifndef _DEBUG
    (void)lvl;
    (void)scope;
    (void)msg;
#else
    if (static_cast<int>(lvl) < g_minLevel.load(std::memory_order_relaxed)) return;

    using clock = std::chrono::system_clock;
    auto     now = clock::now();
    auto     t   = clock::to_time_t(now);
    auto     ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm  tm{};
    localtime_s(&tm, &t);

    char header[128];
    int  hn = std::snprintf(header, sizeof(header),
        "[%02d:%02d:%02d.%03lld][%s][%-16.*s][tid=%05u] ",
        tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(ms),
        kLevelTag[static_cast<int>(lvl)],
        static_cast<int>(scope.size()), scope.data(),
        static_cast<unsigned>(GetCurrentThreadId()));
    if (hn < 0) return;

    {
        std::string combined;
        combined.reserve(static_cast<std::size_t>(hn) + msg.size() + 8);
        combined.append(header, static_cast<std::size_t>(hn));
        combined.append(msg);
        combined.push_back('\n');
        OutputDebugStringA(combined.c_str());
    }

    std::scoped_lock lk(g_sinkMx);
    if (g_stderr && g_stderr != INVALID_HANDLE_VALUE) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        GetConsoleScreenBufferInfo(g_stderr, &info);
        SetConsoleTextAttribute(g_stderr, kLevelColor[static_cast<int>(lvl)]);
        DWORD w = 0;
        WriteFile(g_stderr, header, static_cast<DWORD>(hn), &w, nullptr);
        WriteFile(g_stderr, msg.data(), static_cast<DWORD>(msg.size()), &w, nullptr);
        WriteFile(g_stderr, "\n", 1, &w, nullptr);
        SetConsoleTextAttribute(g_stderr, info.wAttributes);
    }
    if (g_sink) {
        std::fwrite(header, 1, static_cast<size_t>(hn), g_sink);
        std::fwrite(msg.data(), 1, msg.size(), g_sink);
        std::fputc('\n', g_sink);
        std::fflush(g_sink);
    }
#endif
}

void shutdown() noexcept {
    std::scoped_lock lk(g_sinkMx);
    if (g_sink) { std::fclose(g_sink); g_sink = nullptr; }
    g_initDone.store(false, std::memory_order_release);
}

} // namespace vanta::log
