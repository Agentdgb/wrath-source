#include "hitfx_debug.h"
#include "platform/Win32Process.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace vanta::modules::hitfx::dbg {

namespace {

std::mutex          g_mx;
std::FILE*          g_fp = nullptr;
bool                g_tried = false;
char                g_path[512] = {};
unsigned long long  g_t0 = 0;

bool try_open(const char* path)
{
    std::FILE* fp = nullptr;
    fopen_s(&fp, path, "w");
    if (!fp) return false;
    g_fp = fp;
    std::snprintf(g_path, sizeof(g_path), "%s", path);
    return true;
}

void open_once()
{
#ifndef _DEBUG
    g_tried = true;
    return;
#else
    if (g_tried) return;
    g_tried = true;

    const std::wstring dir = vanta::platform::logDir();
    if (!dir.empty()) {
        const std::wstring full = dir + L"\\vanta_hitfx.txt";
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, full.c_str(), L"w") == 0 && fp) {
            g_fp = fp;
            char utf8[512] = {};
            WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
            std::snprintf(g_path, sizeof(g_path), "%s", utf8);
        }
    }

    if (!g_fp) {
        char buf[512]; size_t need = 0;
        if (getenv_s(&need, buf, sizeof(buf), "TEMP") == 0 && need) {
            std::string p(buf); p += "\\vanta_hitfx.txt";
            try_open(p.c_str());
        }
    }

    if (g_fp) {
        g_t0 = GetTickCount64();
        std::fprintf(g_fp,
            "=== gl HitFX debug log ===\n"
            "file: %s  pid=%lu\n"
            "timestamps = ms since this line. Reopen to refresh.\n\n",
            g_path, (unsigned long)GetCurrentProcessId());
        std::fflush(g_fp);
    } else {
        OutputDebugStringA("[vanta_hitfx] FAILED to open any log file\n");
    }
#endif
}

void vlog(const char* fmt, va_list ap)
{
    open_once();
    if (!g_fp) return;
    std::fprintf(g_fp, "[%8llu ms] ", GetTickCount64() - g_t0);
    std::vfprintf(g_fp, fmt, ap);
    std::fputc('\n', g_fp);
    std::fflush(g_fp);
}

} // namespace

void log(const char* fmt, ...)
{
#ifndef _DEBUG
    (void)fmt;
#else
    std::lock_guard<std::mutex> lk(g_mx);
    va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap);
#endif
}

void log_rl(int slot, unsigned min_ms, const char* fmt, ...)
{
#ifndef _DEBUG
    (void)slot; (void)min_ms; (void)fmt;
#else
    static unsigned long long next_ms[32] = {};
    if (slot < 0 || slot >= 32) slot = 0;
    std::lock_guard<std::mutex> lk(g_mx);
    const unsigned long long now = GetTickCount64();
    if (now < next_ms[slot]) return;
    next_ms[slot] = now + min_ms;
    va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap);
#endif
}

} // namespace vanta::modules::hitfx::dbg

