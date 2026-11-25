#include "inject/Injector.h"

#include <windows.h>
#include <chrono>

namespace wrath::inject {

// Strategy implementations (one per compilation unit).
bool load_library(HANDLE, DWORD, std::span<const uint8_t>, const Options&, Result&);
bool map_private(HANDLE, DWORD, std::span<const uint8_t>, const Options&, Result&);
bool map_section_image(HANDLE, DWORD, std::span<const uint8_t>, const Options&, Result&);
bool map_stomp(HANDLE, DWORD, std::span<const uint8_t>, const Options&, Result&);
bool run_thread(HANDLE, DWORD, std::span<const uint8_t>, const Options&, Result&);
bool run_hijack(HANDLE, DWORD, std::span<const uint8_t>, const Options&, Result&);

namespace {

bool valid_pe(std::span<const uint8_t> p) {
    if (p.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(p.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > p.size()) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(p.data() + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE;
}

using Fn = bool(HANDLE, DWORD, std::span<const uint8_t>, const Options&, Result&);
struct Entry { Strategy strategy; Fn* fn; };

// WP-1.1: Auto order — MapPrivate is explicitly excluded from Auto.
constexpr Entry kAutoTable[] = {
    { Strategy::LoadLibrary,     load_library      },
    { Strategy::MapSectionImage, map_section_image },
};

constexpr Strategy kAutoStrategies[] = {
    Strategy::LoadLibrary,
    Strategy::MapSectionImage,
};

// All selectable strategies when requested explicitly.
constexpr Entry kAllTable[] = {
    { Strategy::LoadLibrary,     load_library      },
    { Strategy::MapSectionImage, map_section_image },
    { Strategy::MapPrivate,      map_private       },
    { Strategy::MapStomp,        map_stomp         },
    { Strategy::RunThread,       run_thread        },
};

#ifdef WRATH_SCREENSHARE_PRESET
consteval bool auto_contains_map_private() {
    for (const auto& e : kAutoTable) {
        if (e.strategy == Strategy::MapPrivate) return true;
    }
    return false;
}
static_assert(!auto_contains_map_private(), "x64-screenshare preset forbids MapPrivate in Auto");
#endif

} // namespace

std::span<const Strategy> auto_strategies() {
    return kAutoStrategies;
}

const char* strategy_name(Strategy s) {
    switch (s) {
        case Strategy::Auto:            return "auto";
        case Strategy::LoadLibrary:     return "load-library";
        case Strategy::MapPrivate:      return "map-private";
        case Strategy::MapSectionImage: return "map-section-image";
        case Strategy::MapStomp:        return "map-stomp";
        case Strategy::RunThread:       return "run-thread";
        case Strategy::RunHijack:       return "run-hijack";
    }
    return "unknown";
}

bool parse_strategy(const std::string& name, Strategy* out) {
    if (name == "auto") { *out = Strategy::Auto; return true; }
    for (const auto& e : kAllTable)
        if (name == strategy_name(e.strategy)) { *out = e.strategy; return true; }
    if (name == "run-hijack") { *out = Strategy::RunHijack; return true; }
    return false;
}

Result inject(HANDLE hProc, DWORD pid, std::span<const uint8_t> payload,
              const Options& options) {
    Result result;
    result.target_pid = pid;

    if (!valid_pe(payload)) {
        result.detail = L"inject: payload no es una imagen PE valida.";
        return result;
    }

    if (options.strategy != Strategy::Auto) {
        for (const auto& e : kAllTable) {
            if (e.strategy == options.strategy) {
                Attempt a;
                a.strategy = e.strategy;
                auto t0 = std::chrono::steady_clock::now();
                bool ok = e.fn(hProc, pid, payload, options, result);
                a.ok = ok;
                a.detail = result.detail;
                a.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                result.attempts.push_back(a);
                result.ok = ok;
                result.chosen = e.strategy;
                if (ok) result.effective = e.strategy;
                return result;
            }
        }
        result.detail = L"inject: estrategia solicitada no disponible.";
        return result;
    }

    for (const auto& e : kAutoTable) {
        Attempt a;
        a.strategy = e.strategy;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = e.fn(hProc, pid, payload, options, result);
        a.ok = ok;
        a.detail = result.detail;
        a.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        result.attempts.push_back(a);
        if (ok) {
            result.ok = true;
            result.chosen = e.strategy;
            result.effective = e.strategy;
            return result;
        }
    }

    result.detail = L"inject: agotadas todas las estrategias.";
    return result;
}

} // namespace wrath::inject
