#include "art_background.h"
/* spotify_worker.cpp — SMTC (Windows Media Session) poller for the Spotify
   HUD, spoken to via the plain COM ABI (RoGetActivationFactory + the ABI
   headers under <windows.media.control.h>). Cppwinrt is not used — the
   Windows 10 SDK 10.0.19041 ships an older cppwinrt that fails to compile
   against C++20 in this project.

   The worker:
     - Opens the SMTC session manager.
     - Every second, prefers the Spotify session (matched by AUMID) and
       falls back to whatever session Windows considers current.
     - Reads Title / Artist / Position / Duration / IsPlaying.
     - On track change, tries the session's own thumbnail first, then
       kicks a background iTunes lookup for a HQ 600x600 cover that
       overwrites the low-res thumbnail once it arrives.
     - Decodes the covers, downsamples to build a blurred backdrop, and
       queues both textures for the render thread to upload with real GL. */

#include "spotify_worker.h"
#include "lyrics.h"
#include "platform/StealthThread.h"

#include <nlohmann/json.hpp>

#include <limits>
#include "spotify.h"

#include "platform/ThreadUtil.h"
#include "util/Log.h"
#include "vgui/framework/render/stb/stb_image.hh"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <roapi.h>

/* Linked here rather than in CMake, the way head_cache.cpp links winhttp.
   Keeps the port from touching a shared build file for two libraries only
   this one translation unit needs. */
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "runtimeobject.lib")
#include <winstring.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.media.control.h>
#include <windows.foundation.h>
#include <windows.storage.streams.h>
#include <robuffer.h>   // ::Windows::Storage::Streams::IBufferByteAccess

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dt::modules::spotify::worker {

namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace WMC = ABI::Windows::Media::Control;
namespace WF  = ABI::Windows::Foundation;
namespace WSS = ABI::Windows::Storage::Streams;

using SessionManagerT       = WMC::IGlobalSystemMediaTransportControlsSessionManager;
using SessionManagerStatics = WMC::IGlobalSystemMediaTransportControlsSessionManagerStatics;
using SessionT              = WMC::IGlobalSystemMediaTransportControlsSession;
using MediaPropsT           = WMC::IGlobalSystemMediaTransportControlsSessionMediaProperties;
using TimelinePropsT        = WMC::IGlobalSystemMediaTransportControlsSessionTimelineProperties;
using PlaybackInfoT         = WMC::IGlobalSystemMediaTransportControlsSessionPlaybackInfo;
using SessionsVectorT       = WF::Collections::IVectorView<
    WMC::GlobalSystemMediaTransportControlsSession*>;

std::atomic<bool> g_running{false};
HANDLE             g_thread = nullptr;
HANDLE             g_transport_thread = nullptr;

std::atomic<unsigned int> g_thumb_tex{0};
std::atomic<unsigned int> g_bg_tex{0};
std::atomic<unsigned int> g_ember_tex{0}, g_silk_tex{0};

/* @a second, gentler blur, for the frosted band over the artwork.

   g_bg_tex is deliberately destroyed -- 120 px and a radius of a sixth of the
   side, which is a cloud of colour with no detail left. That is right behind a
   small card and wrong as glass over a photograph: frosted glass is out of
   focus, not erased, and you are still meant to make out what is behind it.
   This one keeps the resolution up and the radius down. */
std::atomic<unsigned int> g_band_tex{0};

std::string wide_to_utf8(const wchar_t* p, size_t n)
{
    if (!p || n == 0) return {};
    const int cb = ::WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(n),
                                         nullptr, 0, nullptr, nullptr);
    if (cb <= 0) return {};
    std::string out(static_cast<size_t>(cb), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(n),
                          out.data(), cb, nullptr, nullptr);
    return out;
}

std::string hstring_to_utf8(HSTRING h)
{
    if (!h) return {};
    UINT32 len = 0;
    const wchar_t* raw = ::WindowsGetStringRawBuffer(h, &len);
    return wide_to_utf8(raw, len);
}

std::wstring utf8_to_wide(std::string_view s)
{
    if (s.empty()) return {};
    const int cch = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                          nullptr, 0);
    if (cch <= 0) return {};
    std::wstring out(static_cast<size_t>(cch), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), cch);
    return out;
}

std::wstring url_encode(std::string_view s, bool plus_for_space = true)
{
    static const char* hex = "0123456789ABCDEF";
    std::wstring out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<wchar_t>(c));
        } else if (c == ' ' && plus_for_space) {
            out.push_back(L'+');
        } else {
            out.push_back(L'%');
            out.push_back(static_cast<wchar_t>(hex[c >> 4]));
            out.push_back(static_cast<wchar_t>(hex[c & 0xF]));
        }
    }
    return out;
}

/* Poll-wait a WinRT IAsyncOperation<T*>-style op via IAsyncInfo::get_Status.
   Avoids the coroutine plumbing cppwinrt.get() needs, which is what breaks
   under this SDK's cppwinrt on C++20. */
template<typename T>
HRESULT sync_op(T* op)
{
    if (!op) return E_POINTER;
    ComPtr<WF::IAsyncInfo> info;
    HRESULT hr = op->QueryInterface(IID_PPV_ARGS(&info));
    if (FAILED(hr)) return hr;
    for (int i = 0; i < 500; ++i) {   // ~5s ceiling
        WF::AsyncStatus status = WF::AsyncStatus::Started;
        info->get_Status(&status);
        if (status == WF::AsyncStatus::Completed) return S_OK;
        if (status == WF::AsyncStatus::Error) {
            HRESULT err = E_FAIL;
            info->get_ErrorCode(&err);
            return err;
        }
        if (status == WF::AsyncStatus::Canceled) return E_ABORT;
        ::Sleep(10);
    }
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

bool http_get(const wchar_t* host, INTERNET_PORT port, const std::wstring& path,
              std::vector<std::uint8_t>& out, size_t cap_bytes = 4 * 1024 * 1024)
{
    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;

    DWORD timeout = 7000;
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

    HINTERNET connection = WinHttpConnect(session, host, port, 0);
    if (!connection) { WinHttpCloseHandle(session); return false; }

    const DWORD flags = (port == INTERNET_DEFAULT_HTTPS_PORT) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) { WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false; }

    bool ok = false;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr)) {

        DWORD status = 0; DWORD sz = sizeof(status);
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                const size_t at = out.size();
                out.resize(at + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, out.data() + at, available, &read)) {
                    out.clear(); break;
                }
                out.resize(at + read);
                if (out.size() > cap_bytes) { out.clear(); break; }
            }
            ok = !out.empty();
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

std::string json_get_string(const std::string& src, const char* key)
{
    const std::string q = std::string{"\""} + key + "\":\"";
    const auto k = src.find(q);
    if (k == std::string::npos) return {};
    const size_t begin = k + q.size();
    const auto end = src.find('"', begin);
    if (end == std::string::npos) return {};
    return src.substr(begin, end - begin);
}

void downsample2x(const std::uint8_t* src, int w, int h,
                  std::vector<std::uint8_t>& dst, int& dw, int& dh)
{
    dw = std::max(1, w / 2);
    dh = std::max(1, h / 2);
    dst.assign(static_cast<size_t>(dw) * dh * 4, 0);
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            int sum[4] = {0, 0, 0, 0};
            int cnt = 0;
            for (int oy = 0; oy < 2; ++oy) {
                const int sy = y * 2 + oy;
                if (sy >= h) continue;
                for (int ox = 0; ox < 2; ++ox) {
                    const int sx = x * 2 + ox;
                    if (sx >= w) continue;
                    const std::uint8_t* p = src + (sy * w + sx) * 4;
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
                    ++cnt;
                }
            }
            std::uint8_t* d = dst.data() + (y * dw + x) * 4;
            d[0] = static_cast<std::uint8_t>(sum[0] / cnt);
            d[1] = static_cast<std::uint8_t>(sum[1] / cnt);
            d[2] = static_cast<std::uint8_t>(sum[2] / cnt);
            d[3] = static_cast<std::uint8_t>(sum[3] / cnt);
        }
    }
}

/* Separable box blur — a running-sum window of size 2r+1 per row (or per
   column) with clamp-to-edge sampling. Three back-to-back passes of a
   box blur approximate a Gaussian, so the sequence
       H(r) V(r) H(r) V(r) H(r) V(r)
   gives a smooth blur without shipping a per-pixel Gaussian kernel. */
void box_blur_pass(std::vector<std::uint8_t>& img, int w, int h, int radius, bool horizontal)
{
    const int stride = w * 4;
    std::vector<std::uint8_t> tmp(img.size());
    const int n_outer = horizontal ? h : w;
    const int n_inner = horizontal ? w : h;

    for (int outer = 0; outer < n_outer; ++outer) {
        auto sample = [&](int i) -> const std::uint8_t* {
            if (i < 0) i = 0;
            if (i >= n_inner) i = n_inner - 1;
            const int y = horizontal ? outer : i;
            const int x = horizontal ? i     : outer;
            return img.data() + y * stride + x * 4;
        };
        auto write = [&](int i, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
            const int y = horizontal ? outer : i;
            const int x = horizontal ? i     : outer;
            std::uint8_t* d = tmp.data() + y * stride + x * 4;
            d[0] = r; d[1] = g; d[2] = b; d[3] = a;
        };

        int sum[4] = {0, 0, 0, 0};
        const int window = 2 * radius + 1;
        for (int k = -radius; k <= radius; ++k) {
            const std::uint8_t* p = sample(k);
            sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
        }
        for (int i = 0; i < n_inner; ++i) {
            write(i,
                  static_cast<std::uint8_t>(sum[0] / window),
                  static_cast<std::uint8_t>(sum[1] / window),
                  static_cast<std::uint8_t>(sum[2] / window),
                  static_cast<std::uint8_t>(sum[3] / window));
            const std::uint8_t* out_p = sample(i - radius);
            const std::uint8_t* in_p  = sample(i + radius + 1);
            sum[0] += in_p[0] - out_p[0];
            sum[1] += in_p[1] - out_p[1];
            sum[2] += in_p[2] - out_p[2];
            sum[3] += in_p[3] - out_p[3];
        }
    }
    img.swap(tmp);
}

void gaussian_ish_blur(std::vector<std::uint8_t>& img, int w, int h, int radius, int passes)
{
    for (int p = 0; p < passes; ++p) {
        box_blur_pass(img, w, h, radius, true);
        box_blur_pass(img, w, h, radius, false);
    }
}

std::string base64_encode(const std::string& in)
{
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t a = static_cast<uint8_t>(in[i]);
        const uint32_t b = (i + 1 < in.size()) ? static_cast<uint8_t>(in[i + 1]) : 0;
        const uint32_t c = (i + 2 < in.size()) ? static_cast<uint8_t>(in[i + 2]) : 0;
        const uint32_t w = (a << 16) | (b << 8) | c;
        out.push_back(tbl[(w >> 18) & 0x3f]);
        out.push_back(tbl[(w >> 12) & 0x3f]);
        out.push_back(i + 1 < in.size() ? tbl[(w >>  6) & 0x3f] : '=');
        out.push_back(i + 2 < in.size() ? tbl[(w      ) & 0x3f] : '=');
    }
    return out;
}

bool http_post(const wchar_t* host, INTERNET_PORT port, const std::wstring& path,
               const std::wstring& extra_headers, const std::string& body,
               std::vector<std::uint8_t>& out, size_t cap_bytes = 512 * 1024)
{
    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    DWORD timeout = 7000;
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);
    HINTERNET connection = WinHttpConnect(session, host, port, 0);
    if (!connection) { WinHttpCloseHandle(session); return false; }
    const DWORD flags = (port == INTERNET_DEFAULT_HTTPS_PORT) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) { WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false; }

    bool ok = false;
    if (WinHttpSendRequest(request,
            extra_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra_headers.c_str(),
            extra_headers.empty() ? 0 : static_cast<DWORD>(-1L),
            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()), 0)
        && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0; DWORD sz = sizeof(status);
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                const size_t at = out.size();
                out.resize(at + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, out.data() + at, available, &read)) {
                    out.clear(); break;
                }
                out.resize(at + read);
                if (out.size() > cap_bytes) { out.clear(); break; }
            }
            ok = !out.empty();
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

/* Spotify Web API access token (Client Credentials flow). Cached in memory
   for `expires_in` seconds; re-requested on demand when the cache expires. */
std::mutex g_token_mx;
std::string g_token;
std::chrono::steady_clock::time_point g_token_expiry;

std::string spotify_get_token(const std::string& client_id, const std::string& client_secret)
{
    if (client_id.empty() || client_secret.empty()) return {};
    {
        std::lock_guard<std::mutex> lk(g_token_mx);
        if (!g_token.empty() && std::chrono::steady_clock::now() < g_token_expiry)
            return g_token;
    }
    const std::string auth = base64_encode(client_id + ":" + client_secret);
    std::wstring headers = L"Content-Type: application/x-www-form-urlencoded\r\n"
                           L"Authorization: Basic " + utf8_to_wide(auth) + L"\r\n";
    const std::string body = "grant_type=client_credentials";
    std::vector<std::uint8_t> resp;
    if (!http_post(L"accounts.spotify.com", INTERNET_DEFAULT_HTTPS_PORT,
                   L"/api/token", headers, body, resp)) return {};
    const std::string json(reinterpret_cast<const char*>(resp.data()), resp.size());
    const std::string tok = json_get_string(json, "access_token");
    if (tok.empty()) return {};
    /* Parse expires_in numeric — cheap scan. */
    long expires = 3600;
    const size_t k = json.find("\"expires_in\":");
    if (k != std::string::npos) {
        long n = 0;
        for (size_t i = k + 13; i < json.size() && (json[i] == ' ' || (json[i] >= '0' && json[i] <= '9')); ++i) {
            if (json[i] >= '0' && json[i] <= '9') n = n * 10 + (json[i] - '0');
        }
        if (n > 60) expires = n;
    }
    std::lock_guard<std::mutex> lk(g_token_mx);
    g_token = tok;
    g_token_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(expires - 30);
    return tok;
}

bool fetch_spotify_cover(const std::string& title, const std::string& artist,
                         const std::string& client_id, const std::string& client_secret,
                         std::vector<std::uint8_t>& out_bytes)
{
    if (title.empty()) return false;
    const std::string token = spotify_get_token(client_id, client_secret);
    if (token.empty()) return false;

    /* Structured search: track:"..." artist:"..." */
    std::string raw_query = "track:\"" + title + "\"";
    if (!artist.empty()) raw_query += " artist:\"" + artist + "\"";
    std::wstring path = L"/v1/search?q=" + url_encode(raw_query) + L"&type=track&limit=1";
    std::wstring headers = L"Authorization: Bearer " + utf8_to_wide(token) + L"\r\n";

    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    DWORD timeout = 7000;
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);
    HINTERNET connection = WinHttpConnect(session, L"api.spotify.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) { WinHttpCloseHandle(session); return false; }
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) { WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false; }

    std::vector<std::uint8_t> resp;
    if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, ssz = sizeof(status);
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &ssz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                const size_t at = resp.size();
                resp.resize(at + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, resp.data() + at, available, &read)) {
                    resp.clear(); break;
                }
                resp.resize(at + read);
                if (resp.size() > 256 * 1024) break;
            }
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (resp.empty()) return false;

    const std::string json(reinterpret_cast<const char*>(resp.data()), resp.size());
    /* The first "url" inside the response is the album's largest image
       (Spotify returns images in decreasing size order). */
    const std::string img_url = json_get_string(json, "url");
    if (img_url.empty()) return false;

    const std::string scheme = "https://";
    if (img_url.rfind(scheme, 0) != 0) return false;
    const size_t host_start = scheme.size();
    const size_t slash = img_url.find('/', host_start);
    if (slash == std::string::npos) return false;
    const std::string host  = img_url.substr(host_start, slash - host_start);
    const std::string ppath = img_url.substr(slash);
    return http_get(utf8_to_wide(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT,
                    utf8_to_wide(ppath), out_bytes);
}

/* @loose equality for catalogue titles.

   The same record is punctuated a dozen ways across services -- "Good Kid,
   M.A.A.d City", "good kid, m.A.A.d city (Deluxe)". Comparing letters and
   digits only, case-folded, matches the ones that are the same record and
   still separates the ones that are not. */
std::string fold_key(const std::string& v)
{
    std::string out;
    out.reserve(v.size());
    for (unsigned char c : v) {
        if (std::isalnum(c))
            out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool fetch_deezer_cover(const std::string& title, const std::string& artist,
                        const std::string& album,
                        std::vector<std::uint8_t>& out_bytes)
{
    if (title.empty()) return false;
    std::string q = "artist:\"" + artist + "\" track:\"" + title + "\"";

    /* @ask for several and choose, instead of asking for one and hoping.

       limit=1 took whatever the catalogue ranked first, which for a song that
       appears on an album, a single and two compilations is a coin toss between
       four different sleeves. */
    std::wstring path = L"/search?q=" + url_encode(q) + L"&limit=12";

    std::vector<std::uint8_t> json_bytes;
    if (!http_get(L"api.deezer.com", INTERNET_DEFAULT_HTTPS_PORT, path, json_bytes)) return false;
    std::string json(reinterpret_cast<const char*>(json_bytes.data()), json_bytes.size());

    /* Walk the "album" objects and keep the one whose title matches what the
       player says is playing. Falls back to the first entry when nothing
       matches, which is exactly the old behaviour. */
    std::string url;
    const std::string want = fold_key(album);

    for (std::size_t p = 0, n = 0; n < 24; ++n) {
        const std::size_t a = json.find("\"album\"", p);
        if (a == std::string::npos) break;

        const std::size_t end = json.find("}", a);
        if (end == std::string::npos) break;

        const std::string entry = json.substr(a, end - a);
        p = end;

        std::string cand = json_get_string(entry, "cover_xl");
        if (cand.empty()) cand = json_get_string(entry, "cover_big");
        if (cand.empty()) continue;

        if (url.empty()) url = cand;          /* first usable, as a fallback */

        if (!want.empty() && fold_key(json_get_string(entry, "title")) == want) {
            url = cand;
            break;
        }
    }

    if (url.empty()) return false;

    /* Deezer's JSON escapes forward slashes as \/ — unescape. */
    for (size_t p = 0; (p = url.find("\\/", p)) != std::string::npos; ) {
        url.replace(p, 2, "/");
    }

    const std::string scheme = "https://";
    if (url.rfind(scheme, 0) != 0) return false;
    const size_t host_start = scheme.size();
    const size_t slash = url.find('/', host_start);
    if (slash == std::string::npos) return false;
    const std::string host  = url.substr(host_start, slash - host_start);
    const std::string ppath = url.substr(slash);
    return http_get(utf8_to_wide(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT,
                    utf8_to_wide(ppath), out_bytes);
}

bool fetch_itunes_cover(const std::string& title, const std::string& artist,
                        const std::string& album,
                        std::vector<std::uint8_t>& out_bytes)
{
    if (title.empty()) return false;
    std::wstring term = url_encode(artist + " " + title);
    std::wstring path = L"/search?term=" + term + L"&entity=song&limit=12";

    std::vector<std::uint8_t> json_bytes;
    if (!http_get(L"itunes.apple.com", INTERNET_DEFAULT_HTTPS_PORT, path, json_bytes)) return false;
    const std::string json(reinterpret_cast<const char*>(json_bytes.data()), json_bytes.size());
    std::string url = json_get_string(json, "artworkUrl100");
    if (url.empty()) return false;

    const std::string needle = "100x100";
    for (size_t p = url.find(needle); p != std::string::npos; p = url.find(needle, p + 1)) {
        url.replace(p, needle.size(), "600x600");
    }

    const std::string scheme = "https://";
    if (url.rfind(scheme, 0) != 0) return false;
    const size_t host_start = scheme.size();
    const size_t slash = url.find('/', host_start);
    if (slash == std::string::npos) return false;
    const std::string host  = url.substr(host_start, slash - host_start);
    const std::string ppath = url.substr(slash);
    return http_get(utf8_to_wide(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT,
                    utf8_to_wide(ppath), out_bytes);
}

/* Read a SMTC thumbnail (IRandomAccessStreamReference) into bytes using
   the plain WSS ABI: OpenReadAsync → IInputStream::ReadAsync into a
   fresh IBuffer built through the BufferFactory activation factory. */
bool read_smtc_thumbnail(WSS::IRandomAccessStreamReference* ref,
                         std::vector<std::uint8_t>& out)
{
    if (!ref) return false;
    ComPtr<WF::IAsyncOperation<WSS::IRandomAccessStreamWithContentType*>> stream_op;
    if (FAILED(ref->OpenReadAsync(&stream_op)) || !stream_op) return false;
    if (FAILED(sync_op(stream_op.Get()))) return false;

    ComPtr<WSS::IRandomAccessStreamWithContentType> stream;
    if (FAILED(stream_op->GetResults(&stream)) || !stream) return false;

    ComPtr<WSS::IRandomAccessStream> ras;
    if (FAILED(stream.As(&ras))) return false;

    UINT64 size = 0;
    ras->get_Size(&size);
    if (size == 0 || size > 4 * 1024 * 1024) return false;

    ComPtr<WSS::IBufferFactory> buffer_factory;
    HStringReference cn_buffer(RuntimeClass_Windows_Storage_Streams_Buffer);
    if (FAILED(::RoGetActivationFactory(cn_buffer.Get(),
                                        IID_PPV_ARGS(&buffer_factory)))) return false;

    ComPtr<WSS::IBuffer> buffer;
    if (FAILED(buffer_factory->Create(static_cast<UINT32>(size), &buffer))) return false;

    ComPtr<WSS::IInputStream> input;
    if (FAILED(ras.As(&input))) return false;

    ComPtr<WF::IAsyncOperationWithProgress<WSS::IBuffer*, UINT32>> read_op;
    if (FAILED(input->ReadAsync(buffer.Get(), static_cast<UINT32>(size),
                                WSS::InputStreamOptions_None, &read_op))) return false;
    if (FAILED(sync_op(read_op.Get()))) return false;

    ComPtr<WSS::IBuffer> read_buffer;
    if (FAILED(read_op->GetResults(&read_buffer)) || !read_buffer) return false;

    UINT32 length = 0;
    read_buffer->get_Length(&length);
    if (length == 0) return false;

    ComPtr<::Windows::Storage::Streams::IBufferByteAccess> ba;
    if (FAILED(read_buffer.CopyTo(IID_PPV_ARGS(&ba)))) return false;

    ::byte* raw = nullptr;
    if (FAILED(ba->Buffer(&raw)) || !raw) return false;

    out.assign(raw, raw + length);
    return true;
}

void decode_and_queue(const std::vector<std::uint8_t>& bytes,
                      std::atomic<unsigned int>* thumb_out,
                      std::atomic<unsigned int>* bg_out,
                      std::atomic<unsigned int>* band_out = nullptr,
                      std::function<bool()> still_current = {})
{
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()), &w, &h, &ch, 4);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return; }

    std::vector<std::uint8_t> full(px, px + static_cast<size_t>(w) * h * 4);
    queue_texture_upload(std::move(full), w, h, thumb_out, still_current);

    /* Three separable passes soften the cover while preserving its broad
       shapes and colors. Built only when artwork changes, off the render thread. */
    std::vector<std::uint8_t> cur(px, px + static_cast<size_t>(w) * h * 4);
    int cw = w, ch2 = h;
    constexpr int kTargetSide = 192;
    while (std::min(cw, ch2) > kTargetSide * 2) {
        std::vector<std::uint8_t> next;
        int nw = 0, nh = 0;
        downsample2x(cur.data(), cw, ch2, next, nw, nh);
        cur = std::move(next);
        cw = nw; ch2 = nh;
    }
    const int blur_radius = std::max(3, std::min(cw, ch2) / 18);
    gaussian_ish_blur(cur, cw, ch2, blur_radius, 3);
    queue_texture_upload(std::move(cur), cw, ch2, bg_out, still_current);

    /* The band copy: about three times the working resolution and a radius an
       order of magnitude tighter, so shapes survive as shapes. */
    if (band_out) {
        std::vector<std::uint8_t> band(px, px + static_cast<size_t>(w) * h * 4);
        int bw = w, bh = h;
        constexpr int kBandSide = 400;
        while (std::min(bw, bh) > kBandSide * 2) {
            std::vector<std::uint8_t> next;
            int nw = 0, nh = 0;
            downsample2x(band.data(), bw, bh, next, nw, nh);
            band = std::move(next);
            bw = nw; bh = nh;
        }
        const int r = std::max(3, std::min(bw, bh) / 22);
        gaussian_ish_blur(band, bw, bh, r, 3);
        queue_texture_upload(std::move(band), bw, bh, band_out, still_current);
    }

    queue_texture_upload(art_background(px,w,h,false),128,128,&g_ember_tex,still_current);
    queue_texture_upload(art_background(px,w,h,true),128,128,&g_silk_tex,still_current);
    stbi_image_free(px);
}

ComPtr<SessionManagerT> acquire_session_manager()
{
    ComPtr<SessionManagerStatics> statics;
    HStringReference cn(RuntimeClass_Windows_Media_Control_GlobalSystemMediaTransportControlsSessionManager);
    if (FAILED(::RoGetActivationFactory(cn.Get(), IID_PPV_ARGS(&statics)))) return {};

    ComPtr<WF::IAsyncOperation<WMC::GlobalSystemMediaTransportControlsSessionManager*>> op;
    if (FAILED(statics->RequestAsync(&op))) return {};
    if (FAILED(sync_op(op.Get()))) return {};

    ComPtr<SessionManagerT> mgr;
    if (FAILED(op->GetResults(&mgr))) return {};
    return mgr;
}

ComPtr<SessionT> pick_session(SessionManagerT* mgr)
{
    if (!mgr) return {};

    ComPtr<SessionsVectorT> sessions;
    if (SUCCEEDED(mgr->GetSessions(&sessions)) && sessions) {
        UINT32 count = 0;
        sessions->get_Size(&count);
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<SessionT> s;
            if (FAILED(sessions->GetAt(i, &s)) || !s) continue;
            HSTRING id_h = nullptr;
            s->get_SourceAppUserModelId(&id_h);
            const std::string id = hstring_to_utf8(id_h);
            ::WindowsDeleteString(id_h);
            if (id.find("Spotify") != std::string::npos) return s;
        }
    }

    ComPtr<SessionT> current;
    mgr->GetCurrentSession(&current);
    return current;
}

/* @lyrics, from LRCLIB.

   Spotify has no public lyrics endpoint — theirs is licensed from Musixmatch
   and lives behind an internal host that wants a browser-session token, not the
   client-credentials one this worker mints. LRCLIB needs no account at all and
   returns LRC, which is the format that carries a timestamp per line. That is
   what makes the highlight follow the vocal instead of trailing it.

   Search rather than the exact-match endpoint: exact match wants the duration,
   and at the instant a track changes SMTC's timeline is often still reporting
   the PREVIOUS song's length. Feeding that to a matcher returns a confident
   "no lyrics" that is indistinguishable from the real thing. */
void fetch_lyrics(const std::string& title, const std::string& artist,
                  const std::string& key, long long duration_ms, const std::string& album)
{
    if (title.empty()) return;

    std::wstring path = L"/api/search?track_name=" + url_encode(title, false);
    if (!artist.empty())
        path += L"&artist_name=" + url_encode(artist, false);

    std::vector<std::uint8_t> body;
    if (!http_get(L"lrclib.net", INTERNET_DEFAULT_HTTPS_PORT, path, body, 2 * 1024 * 1024))
        return;

    if (body.empty()) return;

    const std::string json(reinterpret_cast<const char*>(body.data()), body.size());

    /* Non-throwing: this is third-party data on a detached thread, and an
       exception here would take the process with it. */
    nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_array()) return;

    const nlohmann::json* best = nullptr;
    long long best_gap = (std::numeric_limits<long long>::max)();
    for (const auto& e : doc) {
        if (!e.is_object()) continue;
        const auto synced = e.find("syncedLyrics");
        if (synced == e.end() || !synced->is_string() || synced->get<std::string>().empty()) continue;
        long long gap = 0;
        const auto duration = e.find("duration");
        if (duration_ms > 1000 && duration != e.end() && duration->is_number()) {
            const long long d = static_cast<long long>(duration->get<double>() * 1000.0);
            gap = std::llabs(d - duration_ms);
        }
        if (!best || gap < best_gap) { best = &e; best_gap = gap; }
    }
    if (best) {
        auto parsed = std::make_shared<lyrics_doc>(parse_lrc((*best)["syncedLyrics"].get<std::string>(), key));
        if (!parsed->empty()) {
            spotify::set_lyrics(std::move(parsed));
            VLOG_INFO("spotify", "lyrics loaded for '{}'", title);
            return;
        }
    }

    for (const auto& e : doc) {
        if (!e.is_object()) continue;
        const auto plain = e.find("plainLyrics");
        if (plain == e.end() || !plain->is_string()) continue;
        auto parsed = std::make_shared<lyrics_doc>(parse_plain(plain->get<std::string>(), key));
        if (parsed->empty()) continue;
        spotify::set_lyrics(std::move(parsed));
        return;
    }
    // A valid response with no usable lyrics is a completed empty result.
    auto missing = std::make_shared<lyrics_doc>();
    missing->key = key;
    spotify::set_lyrics(std::move(missing));
}

// Dedicated apartment: metadata/thumbnail reads never block transport commands.
void transport_main()
{
    const HRESULT initialized = ::RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return;
    {
        auto manager = acquire_session_manager();
        struct pending_operation {
            ComPtr<WF::IAsyncOperation<bool>> op;
            std::chrono::steady_clock::time_point started;
        };
        std::vector<pending_operation> pending;
        while (g_running.load(std::memory_order_acquire)) {
            spotify::wait_for_command(10);
            spotify::cmd command{};
            long long argument = 0;
            while (spotify::pop_command(command, argument)) {
                if (!manager) manager = acquire_session_manager();
                auto session = manager ? pick_session(manager.Get()) : ComPtr<SessionT>{};
                if (!session) continue;
                ComPtr<WF::IAsyncOperation<bool>> op;
                HRESULT hr = E_FAIL;
                switch (command) {
                case spotify::cmd::toggle_play: hr = session->TryTogglePlayPauseAsync(&op); break;
                case spotify::cmd::next: hr = session->TrySkipNextAsync(&op); break;
                case spotify::cmd::prev: hr = session->TrySkipPreviousAsync(&op); break;
                case spotify::cmd::shuffle: hr = session->TryChangeShuffleActiveAsync(argument != 0, &op); break;
                case spotify::cmd::seek: {
                    ComPtr<TimelinePropsT> timeline;
                    WF::TimeSpan start{};
                    if (SUCCEEDED(session->GetTimelineProperties(&timeline)) && timeline)
                        timeline->get_StartTime(&start);
                    hr = session->TryChangePlaybackPositionAsync(argument * 10000LL + start.Duration, &op);
                    break;
                }
                }
                if (SUCCEEDED(hr) && op) pending.push_back({op, std::chrono::steady_clock::now()});
                else VLOG_WARN("spotify", "transport command {} rejected", static_cast<int>(command));
            }
            for (auto it = pending.begin(); it != pending.end();) {
                ComPtr<WF::IAsyncInfo> info;
                WF::AsyncStatus status = WF::AsyncStatus::Error;
                if (SUCCEEDED(it->op.As(&info))) info->get_Status(&status);
                const bool timed_out = std::chrono::steady_clock::now() - it->started > std::chrono::seconds(3);
                if (status == WF::AsyncStatus::Started && !timed_out) { ++it; continue; }
                boolean accepted = false;
                if (status == WF::AsyncStatus::Completed) it->op->GetResults(&accepted);
                if (timed_out && info) info->Cancel();
                if (!accepted) VLOG_WARN("spotify", "Player did not accept transport operation");
                it = pending.erase(it);
            }
        }
        for (auto& item : pending) {
            ComPtr<WF::IAsyncInfo> info;
            if (SUCCEEDED(item.op.As(&info))) info->Cancel();
        }
    }
    if (SUCCEEDED(initialized)) ::RoUninitialize();
}

void thread_main()
{
    HRESULT co_hr = ::RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(co_hr) && co_hr != RPC_E_CHANGED_MODE) {
        VLOG_WARN("spotify", "RoInitialize failed: 0x{:x}",
                  static_cast<unsigned>(co_hr));
        return;
    }

    ComPtr<SessionManagerT> mgr = acquire_session_manager();
    if (!mgr) {
        VLOG_WARN("spotify", "SMTC session manager unavailable");
        if (SUCCEEDED(co_hr)) ::RoUninitialize();
        return;
    }

    std::string last_key, last_album;
    ULONGLONG art_retry_at = 0;
    int art_attempts = 0;
    auto art_busy = std::make_shared<std::atomic<bool>>(false);
    auto art_generation = std::make_shared<std::atomic<int>>(0);   // increments per track change

    while (g_running.load(std::memory_order_acquire)) {
        try {
            ComPtr<SessionT> session = pick_session(mgr.Get());
            if (!session) {
                clear_track();
                if (!last_key.empty()) { ++*art_generation; last_key.clear(); last_album.clear(); }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            ComPtr<WF::IAsyncOperation<WMC::GlobalSystemMediaTransportControlsSessionMediaProperties*>> props_op;
            if (FAILED(session->TryGetMediaPropertiesAsync(&props_op)) || !props_op) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            if (FAILED(sync_op(props_op.Get()))) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            ComPtr<MediaPropsT> props;
            if (FAILED(props_op->GetResults(&props)) || !props) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            track_info t;

            HSTRING title_h  = nullptr;
            HSTRING artist_h = nullptr;
            HSTRING album_h  = nullptr;
            props->get_Title(&title_h);
            props->get_Artist(&artist_h);

            /* @the album, which the cover lookup could not do without.

               Searching a catalogue for a title and an artist and taking the
               first hit is how you end up with the single's sleeve, or a
               compilation's, or a live record's -- a different picture of the
               same song. The album name is the field that tells them apart and
               SMTC has been handing it over all along. */
            props->get_AlbumTitle(&album_h);

            t.title  = hstring_to_utf8(title_h);
            t.artist = hstring_to_utf8(artist_h);
            t.album  = hstring_to_utf8(album_h);

            ::WindowsDeleteString(title_h);
            ::WindowsDeleteString(artist_h);
            ::WindowsDeleteString(album_h);

            ComPtr<TimelinePropsT> timeline;
            if (SUCCEEDED(session->GetTimelineProperties(&timeline)) && timeline) {
                WF::TimeSpan pos{}, end{}, start{};
                timeline->get_Position(&pos);
                timeline->get_EndTime(&end);
                timeline->get_StartTime(&start);
                /* TimeSpan.Duration is in 100-ns ticks; /10000 = ms. */
                const long long pos_ms   = pos.Duration   / 10000;
                const long long end_ms   = end.Duration   / 10000;
                const long long start_ms = start.Duration / 10000;
                t.position_ms = pos_ms - start_ms;
                t.duration_ms = end_ms - start_ms;
                /* Kept: SMTC reports position relative to this, so a seek has
                   to add it back before it goes out. It was being computed and
                   thrown away. */
                t.start_ms    = start_ms;


                /* @stamped HERE, where the position was actually sampled.

                   It used to be stamped at the very end of the poll, several
                   hundred milliseconds later — after the media-properties round
                   trip and the cover work. The render thread extrapolates
                   forward from this mark, so a late stamp meant it added time
                   that had already elapsed and every position it computed ran
                   ahead of the song. Harmless on a progress bar; on a lyrics
                   view it is the whole complaint. */

                /* @and back-dated to when the PLAYER pushed it, not to when we
                   read it.

                   SMTC's Position is not live. It is the last value the source
                   app handed to the session, and the session records when that
                   happened in LastUpdatedTime. Spotify publishes a timeline
                   update only every so often, so by the time this loop reads
                   Position it can already be seconds old -- and stamping it
                   with `now` claimed it had just been sampled. That claim is
                   the delay: the render clock projects forward from this mark,
                   so every position it computed was short by exactly the
                   staleness, on every frame, for as long as the song played.

                   DateTime::UniversalTime and FILETIME share an epoch -- 100-ns
                   ticks since 1601-01-01 UTC -- so the two subtract directly.
                   Both are wall clock and snapshot_ns is steady_clock, so the
                   difference is applied as an OFFSET to the steady reading and
                   never converted between the two.

                   The guards, in order:
                     - UniversalTime <= 0: the session has never published a
                       timeline. Fall back to stamping now, which is exactly
                       what this line did before.
                     - A negative difference: the stamp is in our future, which
                       means the wall clock moved between the push and this
                       read. Treat it as fresh. Never stamp forward -- the
                       renderer would then project a position the song has not
                       reached yet, which is the failure the old comment above
                       describes from the other direction.
                     - Beyond the cap: either the player has genuinely
                       stopped publishing, or the wall clock stepped (NTP
                       correction, resume from sleep, a VM restore) while
                       steady_clock did not. Either way the number is not a
                       staleness, so it is thrown away and the stamp falls back
                       to now. */
                constexpr long long k_max_stale_ms = 10'000;

                const long long now_steady_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();

                long long stale_ns = 0;
                WF::DateTime updated{};
                if (SUCCEEDED(timeline->get_LastUpdatedTime(&updated)) &&
                    updated.UniversalTime > 0) {

                    FILETIME ft{};
                    ::GetSystemTimePreciseAsFileTime(&ft);

                    ULARGE_INTEGER uli;
                    uli.LowPart  = ft.dwLowDateTime;
                    uli.HighPart = ft.dwHighDateTime;

                    const long long d_ticks =
                        static_cast<long long>(uli.QuadPart) - updated.UniversalTime;

                    if (d_ticks > 0) {
                        const long long d_ms = d_ticks / 10000;   /* 100-ns ticks -> ms */

                        /* Past the cap the number is REJECTED, not clamped.
                           Clamping would publish a stamp we do not believe, and
                           the renderer believes every stamp: it would pin `age`
                           at ten seconds while `display` kept free-running, so
                           the error would walk down a millisecond per
                           millisecond until it tripped the snap and jumped
                           backwards -- once per sample, for ever. Falling back
                           to `now` is the behaviour this line had before the
                           back-date existed, which is merely late rather than
                           actively wrong. */
                        if (d_ms <= k_max_stale_ms)
                            stale_ns = d_ms * 1'000'000LL;
                    }
                }

                t.snapshot_ns = now_steady_ns - stale_ns;

                /* steady_clock is boot-relative on Windows, so a back-date taken
                   within the first ten seconds of uptime could cross zero -- and
                   zero is the renderer's "no sample" sentinel (spotify.cpp:1102),
                   which would silently stop it correcting. */
                if (t.snapshot_ns <= 0) t.snapshot_ns = now_steady_ns;
                if (t.duration_ms < 0) t.duration_ms = 0;
                if (t.position_ms < 0) t.position_ms = 0;
            }

            ComPtr<PlaybackInfoT> playback;
            if (SUCCEEDED(session->GetPlaybackInfo(&playback)) && playback) {
                WMC::GlobalSystemMediaTransportControlsSessionPlaybackStatus status{};
                playback->get_PlaybackStatus(&status);
                t.playing =
                    (status == WMC::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Playing);

                /* @ask before drawing a slider that might do nothing.

                   Not every player honours a seek over SMTC, and a bar that
                   swallows a drag in silence is worse than one that is plainly
                   not draggable. */
                ComPtr<WMC::IGlobalSystemMediaTransportControlsSessionPlaybackControls> ctl;
                if (SUCCEEDED(playback->get_Controls(&ctl)) && ctl) {
                    boolean v = 0;
                    if (SUCCEEDED(ctl->get_IsPlaybackPositionEnabled(&v)))
                        t.can_seek = (v != 0);

                    boolean sh = 0;
                    if (SUCCEEDED(ctl->get_IsShuffleEnabled(&sh)))
                        t.can_shuffle = (sh != 0);
                }

                /* Current shuffle state, so the button can show whether it is
                   on rather than just that it exists. */
                ComPtr<WF::IReference<bool>> shuffle_ref;
                if (SUCCEEDED(playback->get_IsShuffleActive(&shuffle_ref)) && shuffle_ref) {
                    boolean on = 0;
                    if (SUCCEEDED(shuffle_ref->get_Value(&on)))
                        t.shuffled = (on != 0);
                }

            }

            const std::string key = t.title + "\x1f" + t.artist;
            const bool changed = key != last_key || t.album != last_album;
            const bool retry_art = !changed && !t.title.empty() && g_thumb_tex.load()==0
                && !art_busy->load() && art_attempts < 3 && GetTickCount64() >= art_retry_at;

            if ((changed || retry_art) && !t.title.empty()) {
                if (changed) art_attempts = 0;
                ++art_attempts; art_retry_at = GetTickCount64() + 10000;
                last_key = key; last_album = t.album;
                const int gen = art_generation->fetch_add(1) + 1;
                g_thumb_tex.store(0, std::memory_order_release);
                g_bg_tex.store(0, std::memory_order_release);
                g_band_tex.store(0, std::memory_order_release);
                g_ember_tex.store(0); g_silk_tex.store(0);

                /* Snapshot the settings before we detach, so the network
                   thread doesn't reach into the module while the render
                   thread is publishing new values. */
                const settings sv = get_settings_snapshot();

                /* Dropped now so the panel never shows the last song's words
                   while the new ones are in flight. */
                if (changed) spotify::begin_lyrics(key);

                if (changed) {
                    const std::string lt = t.title, la = t.artist, lk = key, lal = t.album;
                    const long long   ld = t.duration_ms;

                    dt::platform::spawnNamed("Audio Worker", [lt, la, lk, ld, gen, lal] {
                        if (!g_running.load(std::memory_order_acquire)) return;
                        try {
                            fetch_lyrics(lt, la, lk, ld, lal);
                        } catch (...) {
                        }
                        (void)gen;
                    });
                }

                /* Cover pipeline runs on a reaper-tracked thread so unload (F8)
                   never frees the DLL while WinHTTP is still in flight. */
                ComPtr<WSS::IRandomAccessStreamReference> thumb_ref;
                props->get_Thumbnail(&thumb_ref);
                std::vector<std::uint8_t> smtc_bytes;
                if (thumb_ref) read_smtc_thumbnail(thumb_ref.Get(), smtc_bytes);

                art_busy->store(true);
                dt::platform::spawnNamed("Image Decoder", [title = t.title, artist = t.artist, album = t.album,
                             sv, smtc_bytes = std::move(smtc_bytes),
                             gen, gen_ref = art_generation, art_busy]() mutable {
                    struct Done { std::shared_ptr<std::atomic<bool>> busy; ~Done() { busy->store(false); } } done{art_busy};
                    try {
                    if (!g_running.load(std::memory_order_acquire)) return;
                    auto publish = [&](std::vector<std::uint8_t>& bytes) {
                        if (!g_running.load(std::memory_order_acquire)) return true;
                        if (gen_ref->load(std::memory_order_acquire) != gen) return true;
                        int iw=0, ih=0, ic=0;
                        if (bytes.empty() || !stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &iw, &ih, &ic)
                            || iw < 32 || ih < 32 || iw > 8192 || ih > 8192) return false;
                        decode_and_queue(bytes, &g_thumb_tex, &g_bg_tex, &g_band_tex,
                            [gen_ref, gen] { return gen_ref->load(std::memory_order_acquire) == gen; });
                        return true;
                    };

                    // Prefer the actual media-session cover over catalogue guesses.
                    if (!smtc_bytes.empty()) {
                        int iw=0, ih=0, ic=0;
                        if (stbi_info_from_memory(smtc_bytes.data(), static_cast<int>(smtc_bytes.size()), &iw, &ih, &ic)
                            && std::min(iw,ih)>=64 && std::abs(iw-ih)<=std::min(iw,ih)/8
                            && publish(smtc_bytes)) return;
                    }
                    std::vector<std::uint8_t> bytes;

                    if (g_running.load(std::memory_order_acquire) &&
                        fetch_spotify_cover(title, artist,
                            sv.spotify_client_id, sv.spotify_client_secret, bytes)) {
                        if (publish(bytes)) return;
                    }
                    bytes.clear();

                    if (g_running.load(std::memory_order_acquire) &&
                        fetch_deezer_cover(title, artist, album, bytes)) {
                        if (publish(bytes)) return;
                    }
                    bytes.clear();

                    if (g_running.load(std::memory_order_acquire) &&
                        fetch_itunes_cover(title, artist, album, bytes)) {
                        if (publish(bytes)) return;
                    }

                    /* Last-resort: SMTC thumbnail if it looks square. */
                    if (g_running.load(std::memory_order_acquire) && !smtc_bytes.empty()) {
                        int iw = 0, ih = 0, ic = 0;
                        if (stbi_info_from_memory(smtc_bytes.data(),
                                static_cast<int>(smtc_bytes.size()), &iw, &ih, &ic)) {
                            const int mn = std::min(iw, ih);
                            const int diff = std::abs(iw - ih);
                            if (mn > 0 && diff <= mn / 8) {
                                publish(smtc_bytes);
                            }
                        }
                    }
                    } catch (...) { /* A later bounded retry can recover. */ }
                });
            }

            t.thumb_tex_id      = g_thumb_tex.load(std::memory_order_acquire);
            t.blurred_bg_tex_id = g_bg_tex.load(std::memory_order_acquire);
            t.band_tex_id       = g_band_tex.load(std::memory_order_acquire);
            t.ember_tex_id = g_ember_tex.load(); t.silk_tex_id = g_silk_tex.load();
            /* NOT re-stamped: t.snapshot_ns was set next to the position read
               above, which is the instant it describes. Only filled in here if
               the timeline was unavailable and nothing set it. */
            if (t.snapshot_ns == 0) {
                t.snapshot_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            }
            set_track(t);
        } catch (...) {
        }

        // Refresh displayed state promptly; the command thread owns the wakeable queue.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }

    ++*art_generation; // Discard uploads still in flight when this worker stops.
    if (SUCCEEDED(co_hr)) ::RoUninitialize();
}

}

void start()
{
    if (g_running.exchange(true, std::memory_order_acq_rel)) return;
    g_thread = dt::platform::createStealthThread([](LPVOID) -> DWORD {
        thread_main();
        return 0;
    }, nullptr);
    g_transport_thread = dt::platform::createStealthThread([](LPVOID) -> DWORD {
        transport_main();
        return 0;
    }, nullptr);
}

void stop()
{
    g_running.store(false, std::memory_order_release);
    if (g_transport_thread) {
        WaitForSingleObject(g_transport_thread, 2000);
        CloseHandle(g_transport_thread);
        g_transport_thread = nullptr;
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    spotify::cmd discarded{}; long long argument = 0;
    while (spotify::pop_command(discarded, argument)) {}
    g_thumb_tex.store(0);
    g_bg_tex.store(0);
    g_band_tex.store(0);
}

}
