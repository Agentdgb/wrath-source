#include "head_cache.h"

#include "platform/ThreadUtil.h"
#include "util/Log.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <winhttp.h>
#include <gl/GL.h>

#include "vgui/framework/render/stb/stb_image.hh"

#pragma comment(lib, "winhttp.lib")

namespace vanta::modules::targethud::heads {

namespace {

/* A fight involves a handful of people and a session a few dozen. The cap is
   here so a crowded server cannot turn a HUD into an unbounded texture leak, not
   because the expected count is anywhere near it. */
constexpr std::size_t k_max_entries = 48;

enum class state { pending, ready, missing };

struct entry
{
    state state = state::pending;

    std::vector<std::uint8_t> pixels{};   /* RGBA, handed from the worker */
    int width = 0;
    int height = 0;

    unsigned int texture = 0;             /* render thread only */
    std::uint64_t used = 0;               /* for eviction */
};

std::mutex g_lock;
std::unordered_map<std::string, entry> g_entries;
std::deque<std::string> g_queue;

std::condition_variable g_wake;
std::atomic<bool> g_running{ false };

/* @a one-way latch, separate from g_running.

   g_running is a running-state flag: shutdown clears it, and anything that
   tested only that flag would happily start a second worker afterwards. This
   one never goes back, so a draw that lands after shutdown cannot re-arm the
   fetcher into a cache that has already been torn down. */
std::atomic<bool> g_shut_down{ false };

std::uint64_t g_clock = 0;

bool http_get(const wchar_t* host, const std::wstring& path, std::vector<std::uint8_t>& out)
{
    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (session == nullptr)
        return false;

    /* Shorter than PlayerProfile's five seconds. That one asks about a single
       name and can afford to wait; this one is asked about every player you look
       at, and on a LAN or offline server every one of them fails — the timeout
       is then the whole cost of the feature, paid repeatedly. */
    DWORD timeout = 2500;
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);

    if (connection == nullptr)
    {
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    if (request == nullptr)
    {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    bool ok = false;

    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr))
    {
        DWORD status = 0;
        DWORD size = sizeof(status);

        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

        if (status == 200)
        {
            DWORD available = 0;

            while (WinHttpQueryDataAvailable(request, &available) && available > 0)
            {
                const std::size_t at = out.size();
                out.resize(at + available);

                DWORD read = 0;

                if (!WinHttpReadData(request, out.data() + at, available, &read))
                {
                    out.clear();
                    break;
                }

                out.resize(at + read);

                /* Checked inside the loop, not just between requests. A fetch
                   against an unreachable host is where the shutdown latency
                   lives, and a chunk boundary is the only place this can be
                   noticed without an abortable handle. */
                if (!g_running.load(std::memory_order_acquire))
                {
                    out.clear();
                    break;
                }

                if (out.size() > 512 * 1024)
                {
                    out.clear();
                    break;
                }
            }

            ok = !out.empty();
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    return ok;
}

std::wstring widen(const std::string& value)
{
    std::wstring out;
    out.reserve(value.size());

    for (const char character : value)
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));

    return out;
}

bool fetch(const std::string& username, std::vector<std::uint8_t>& pixels, int& width, int& height)
{
    /* The composited head — face plus the hat layer — rather than the raw skin,
       so the HUD does not have to know the 64x64 skin layout or composite two
       8x8 regions itself. Two providers, because one of them being down should
       not mean the HUD has no heads. */
    struct provider { const wchar_t* host; const wchar_t* prefix; const wchar_t* suffix; };

    static const provider providers[] = {
        { L"mc-heads.net", L"/avatar/", L"/64.png" },
        { L"minotar.net",  L"/helm/",   L"/64.png" },
    };

    const std::wstring name = widen(username);

    for (const provider& source : providers)
    {
        if (!g_running.load(std::memory_order_acquire))
            return false;

        std::vector<std::uint8_t> png;

        std::wstring path = source.prefix;
        path += name;
        path += source.suffix;

        if (!http_get(source.host, path, png))
            continue;

        int channels = 0;
        unsigned char* decoded = stbi_load_from_memory(
            png.data(), static_cast<int>(png.size()), &width, &height, &channels, 4);

        if (decoded == nullptr || width < 1 || height < 1)
        {
            if (decoded != nullptr) stbi_image_free(decoded);
            continue;
        }

        pixels.assign(decoded, decoded + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(decoded);

        return true;
    }

    return false;
}

void worker_body()
{
    while (g_running.load(std::memory_order_acquire))
    {
        std::string job;

        {
            std::unique_lock<std::mutex> guard(g_lock);

            g_wake.wait(guard, [] { return !g_queue.empty() || !g_running.load(std::memory_order_acquire); });

            if (!g_running.load(std::memory_order_acquire))
                return;

            job = std::move(g_queue.front());
            g_queue.pop_front();
        }

        std::vector<std::uint8_t> pixels;
        int width = 0;
        int height = 0;

        const bool ok = fetch(job, pixels, width, height);

        {
            std::lock_guard<std::mutex> guard(g_lock);

            auto found = g_entries.find(job);

            if (found == g_entries.end())
                continue;   /* evicted while we were away */

            if (ok)
            {
                found->second.pixels = std::move(pixels);
                found->second.width = width;
                found->second.height = height;
                found->second.state = state::ready;
            }
            else
            {
                /* @remembered as missing, not retried.

                   A single-slot fetcher can afford a backoff chain because it
                   asks about one name. A per-name one cannot: an offline session
                   or a LAN server means every name fails, and retrying each of
                   them turns a HUD into a request storm against a third party
                   that never owed us anything. */
                found->second.state = state::missing;
            }
        }
    }
}

void ensure_worker()
{
    if (g_shut_down.load(std::memory_order_acquire))
        return;

    if (g_running.exchange(true, std::memory_order_acq_rel))
        return;

    /* @spawnNamed, not std::thread.

       Two reasons, both fatal the other way. A namespace-scope std::thread is
       destroyed at static destruction, and destroying a joinable one calls
       std::terminate — so every path would have to guarantee a join, including
       the ones that do not run. And joining here would mean joining from inside
       the wglSwapBuffers detour: an eject during a fetch against a dead host
       would freeze the game's render thread until the request timed out, long
       enough for the unload watchdog to give up and tear the hooks out from
       under a thread still inside them.

       spawnNamed hands the thread to the client's own reaper, which already
       holds the unload open until every one of them has returned. Nothing here
       joins, and nothing here can be left joinable. */
    vanta::platform::spawnNamed("Asset Loader", &worker_body);
}

void evict_if_needed()
{
    if (g_entries.size() <= k_max_entries)
        return;

    auto oldest = g_entries.end();

    for (auto it = g_entries.begin(); it != g_entries.end(); ++it)
    {
        if (it->second.state == state::pending)
            continue;   /* a fetch is in flight for it; the worker still holds the key */

        if (oldest == g_entries.end() || it->second.used < oldest->second.used)
            oldest = it;
    }

    if (oldest == g_entries.end())
        return;

    if (oldest->second.texture != 0)
        glDeleteTextures(1, &oldest->second.texture);

    g_entries.erase(oldest);
}

} // namespace

ImTextureID texture_for(const std::string& username)
{
    if (username.empty())
        return 0;

    ensure_worker();

    unsigned int upload_texture = 0;
    std::vector<std::uint8_t> upload_pixels;
    int upload_w = 0;
    int upload_h = 0;

    {
        std::lock_guard<std::mutex> guard(g_lock);

        ++g_clock;

        auto found = g_entries.find(username);

        if (found == g_entries.end())
        {
            entry fresh{};
            fresh.used = g_clock;

            g_entries.emplace(username, std::move(fresh));
            g_queue.push_back(username);

            evict_if_needed();

            g_wake.notify_one();
            return 0;
        }

        found->second.used = g_clock;

        if (found->second.state == state::missing)
            return 0;

        if (found->second.texture != 0)
            return static_cast<ImTextureID>(found->second.texture);

        if (found->second.state != state::ready)
            return 0;

        /* COPIED, not moved. The upload happens outside the lock — glTexImage2D
           is a driver call and holding the queue lock across one would stall the
           worker for no reason — but that means it can still fail after this
           block, and moving the pixels out would leave the entry marked ready
           with no texture and nothing left to build one from: a head that never
           appears and is never retried. The copy is 16KB and happens once per
           name. */
        upload_pixels = found->second.pixels;
        upload_w = found->second.width;
        upload_h = found->second.height;
    }

    if (upload_pixels.empty() || upload_w < 1 || upload_h < 1)
        return 0;

    glGenTextures(1, &upload_texture);

    if (upload_texture == 0)
        return 0;

    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);

    glBindTexture(GL_TEXTURE_2D, upload_texture);

    /* NEAREST. A Minecraft head is 8x8 pixel art blown up to 64; a bilinear
       filter turns the face into a smudge. PlayerProfile made the same call for
       the same reason. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F /*GL_CLAMP_TO_EDGE*/);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, upload_w, upload_h, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, upload_pixels.data());

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));

    {
        std::lock_guard<std::mutex> guard(g_lock);

        auto found = g_entries.find(username);

        if (found == g_entries.end())
        {
            /* evicted between the unlock and here — do not leak the texture we
               just made for a row that no longer exists */
            glDeleteTextures(1, &upload_texture);
            return 0;
        }

        found->second.texture = upload_texture;

        /* The pixels have done their job. Released only now that a texture
           exists to replace them. */
        found->second.pixels.clear();
        found->second.pixels.shrink_to_fit();
    }

    return static_cast<ImTextureID>(upload_texture);
}

void shutdown()
{
    g_shut_down.store(true, std::memory_order_release);

    /* @the flag is published UNDER the lock.

       The waiter evaluates `!g_queue.empty() || !g_running` while holding
       g_lock. Clearing g_running outside it opens a window between that
       evaluation and the wait itself in which the notify lands on nobody — the
       worker then sleeps through its own shutdown, and the reaper waits on a
       thread that will never wake. */
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_running.store(false, std::memory_order_release);
    }

    g_wake.notify_all();

    std::lock_guard<std::mutex> guard(g_lock);

    for (auto& pair : g_entries)
    {
        if (pair.second.texture != 0)
            glDeleteTextures(1, &pair.second.texture);
    }

    g_entries.clear();
    g_queue.clear();
}

} // namespace vanta::modules::targethud::heads
