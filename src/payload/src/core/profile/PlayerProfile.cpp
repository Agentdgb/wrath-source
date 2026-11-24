#include "core/profile/PlayerProfile.h"

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "platform/ThreadUtil.h"
#include "util/Log.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

// Header only — the implementation is compiled once, in
// vgui/framework/render/render.cc. Defining it again here would give the linker
// two copies of every stbi_ symbol.
#include "vgui/framework/render/stb/stb_image.hh"

#pragma comment(lib, "winhttp.lib")

namespace vanta::profile {

namespace {

std::mutex               g_lock;
std::string              g_name;
std::vector<std::uint8_t> g_head;
int                      g_head_w = 0;
int                      g_head_h = 0;
std::atomic<bool>        g_head_ready{false};
std::atomic<bool>        g_running{false};

// ---------------------------------------------------------------------------

// MinecraftClient.getInstance().getSession().getUsername()
//
// Deliberately the session and not the player: the player object only exists
// once a world is loaded, so keying off it would leave the card blank in the
// main menu — which is exactly where someone is most likely to open the gui.
std::string readNameFromJvm() {
    if (!vanta::jvm::ctx().vm.load()) return {};

    vanta::jvm::JniScope scope;
    JNIEnv* env = scope.get();
    if (!env) return {};

    vanta::jvm::LocalObject mc = vanta::jvm::staticCallObjectCanon(env, "MinecraftClient", "getInstance");
    if (!mc) return {};

    vanta::jvm::LocalObject session = vanta::jvm::callObjectCanon(env, mc.get(), "MinecraftClient", "getSession");
    if (!session) return {};

    vanta::jvm::LocalObject name = vanta::jvm::callObjectCanon(env, session.get(), "Session", "getUsername");
    if (!name) return {};

    auto str = static_cast<jstring>(name.get());
    const char* utf = env->GetStringUTFChars(str, nullptr);
    if (!utf) return {};

    std::string result(utf);
    env->ReleaseStringUTFChars(str, utf);

    return result;
}

// ---------------------------------------------------------------------------

// A plain HTTPS GET. WinHTTP rather than WinINet because this runs on a worker
// thread with no message loop and no user session assumptions.
bool httpGet(const wchar_t* host, const std::wstring& path, std::vector<std::uint8_t>& out) {
    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    // Nothing here is worth hanging the worker over.
    DWORD timeout = 5000;
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) { WinHttpCloseHandle(session); return false; }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    bool ok = false;

    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr)) {

        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                const std::size_t at = out.size();
                out.resize(at + available);

                DWORD read = 0;
                if (!WinHttpReadData(request, out.data() + at, available, &read)) {
                    out.clear();
                    break;
                }
                out.resize(at + read);

                // A head PNG is a couple of KB. Anything this large is not one.
                if (out.size() > 512 * 1024) { out.clear(); break; }
            }
            ok = !out.empty();
        } else if (status != 0) {
            VLOG_WARN("profile", "skin service answered HTTP {}", static_cast<int>(status));
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    return ok;
}

bool fetchHead(const std::string& username) {
    // The rendered head (face + hat overlay composited) at 64px. Asking the
    // service for the composite rather than the raw skin keeps us out of the
    // business of knowing where the face sits in a skin sheet, which differs
    // between the classic 64x32 layout and the modern 64x64 one.
    //
    // Multiple providers with the same path convention (helm-style):
    //   mc-heads.net  → /avatar/<name>/<size>
    //   minotar.net   → /helm/<name>/<size>
    //   crafatar (via .com fallback) uses UUID, skipped here.
    // Any that resolves and returns 200 wins; a firewall or blocked service
    // simply falls through instead of leaving the card on the placeholder
    // Steve icon indefinitely.
    struct Provider {
        const wchar_t* host;
        std::wstring   path_prefix;   // includes leading '/'
        std::wstring   path_suffix;   // includes leading '/' before size
    };

    std::wstring wname;
    wname.append(username.begin(), username.end());

    const Provider providers[] = {
        { L"mc-heads.net",  L"/avatar/", L"/128.png" },
        { L"minotar.net",   L"/helm/",   L"/64.png"  },
        { L"crafatar.com",  L"/avatars/", L"?size=128&overlay" }, // works with UUID; leave as best-effort
    };

    for (const auto& prov : providers) {
        std::wstring path = prov.path_prefix + wname + prov.path_suffix;

        std::vector<std::uint8_t> png;
        if (!httpGet(prov.host, path, png)) {
            VLOG_INFO("profile", "skin service unreachable, trying next");
            continue;
        }

        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load_from_memory(
            png.data(), static_cast<int>(png.size()), &w, &h, &channels, 4);

        if (!pixels || w <= 0 || h <= 0) {
            if (pixels) stbi_image_free(pixels);
            VLOG_WARN("profile", "skin service returned garbage for '{}', trying next", username);
            continue;
        }

        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_head.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 4);
            g_head_w = w;
            g_head_h = h;
        }

        stbi_image_free(pixels);
        g_head_ready.store(true, std::memory_order_release);

        VLOG_INFO("profile", "head fetched for '{}' ({}x{})", username, w, h);
        return true;
    }

    return false;
}

} // namespace

void start() {
    if (g_running.exchange(true)) return;

    vanta::platform::spawnNamed("HTTP Worker", []{
        /* The JVM may not be up yet — bootstrap arms the overlay before it
           touches Java on purpose — and in a launcher the session is populated
           after auth. So poll rather than read once and give up.

           Tight at first, then slack. The overlay takes about a second to come
           up, so the first few hundred milliseconds are the whole budget for
           having the name ready BEFORE the panel is ever drawn; after that we
           are only waiting on a login and there is no point spinning. */
        std::string username;
        for (int i = 0; i < 160 && g_running.load(); ++i) {
            username = readNameFromJvm();
            if (!username.empty()) break;
            Sleep(i < 50 ? 20 : 500);
        }

        if (username.empty()) {
            VLOG_WARN("profile", "could not read the account name from the JVM");
            return;
        }

        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_name = username;
        }
        VLOG_INFO("profile", "signed in as '{}'", username);

        if (!g_running.load()) return;

        // Best effort with a bounded retry chain. Some networks race the DNS
        // for the skin providers on first launch (or block one specific host
        // and let the next through), so a single miss should not doom the
        // card to the placeholder Steve for the whole session.
        //
        // Retries: 0s → 3s → 10s → 30s → 60s → 120s → 300s, then stop trying.
        static const int backoff_ms[] = { 0, 3000, 10000, 30000, 60000, 120000, 300000 };

        bool ok = false;
        for (int attempt = 0; attempt < static_cast<int>(std::size(backoff_ms)); ++attempt) {
            const int wait_ms = backoff_ms[attempt];

            // Small-slice sleep so a shutdown can wake us out of the wait.
            int remaining = wait_ms;
            while (remaining > 0 && g_running.load()) {
                const int step = remaining > 250 ? 250 : remaining;
                Sleep(step);
                remaining -= step;
            }
            if (!g_running.load()) return;

            if (fetchHead(username)) { ok = true; break; }
            VLOG_WARN("profile", "skin head fetch attempt {} failed for '{}', retrying",
                attempt + 1, username);
        }

        if (!ok)
            VLOG_WARN("profile", "could not fetch the skin head for '{}' after all retries", username);
    });
}

void stop() {
    g_running.store(false);

    std::lock_guard<std::mutex> guard(g_lock);
    g_head.clear();
    g_head_w = g_head_h = 0;
    g_head_ready.store(false, std::memory_order_release);
}

std::string name() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_name;
}

bool hasPendingHead() {
    return g_head_ready.load(std::memory_order_acquire);
}

bool takeHead(std::vector<std::uint8_t>& rgba, int& width, int& height) {
    if (!g_head_ready.exchange(false, std::memory_order_acq_rel)) return false;

    std::lock_guard<std::mutex> guard(g_lock);
    if (g_head.empty()) return false;

    rgba.swap(g_head);
    width  = g_head_w;
    height = g_head_h;

    g_head.clear();
    return true;
}

} // namespace vanta::profile
