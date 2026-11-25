// Verifies the injection API contract without touching a real process:
//  - all strategies parse and round-trip through strategy_name()
//  - Auto excludes MapPrivate and tries LoadLibrary then MapSectionImage
//  - Result::effective is unset on failure and reflects the real strategy on success
//  - --strategy=load-library with --no-unlink (unlink=false) is accepted and noted
#include "inject/Injector.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace wrath::inject;

static bool check(bool cond, const char* what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    return cond;
}

int main() {
    bool ok = true;

    const char* names[] = { "load-library", "map-private", "map-section-image", "map-stomp", "run-thread", "run-hijack" };
    for (const char* n : names) {
        Strategy s{};
        bool parsed = parse_strategy(n, &s);
        ok &= check(parsed, n);
        if (parsed) ok &= check(std::string(strategy_name(s)) == n, "round-trip");
    }

    // (a) Auto must not contain MapPrivate; it must contain LoadLibrary and MapSectionImage.
    auto auto_list = auto_strategies();
    bool has_map_private = false;
    bool has_load_lib = false;
    bool has_sec_img = false;
    for (Strategy s : auto_list) {
        if (s == Strategy::MapPrivate) has_map_private = true;
        if (s == Strategy::LoadLibrary) has_load_lib = true;
        if (s == Strategy::MapSectionImage) has_sec_img = true;
    }
    ok &= check(!has_map_private, "Auto excludes MapPrivate");
    ok &= check(has_load_lib && has_sec_img, "Auto includes LoadLibrary and MapSectionImage");

    std::vector<uint8_t> bad(64, 0);
    Result r = inject(nullptr, 1234, bad, Options{});
    ok &= check(!r.ok, "invalid PE rejected");
    ok &= check(!r.effective.has_value(), "invalid PE has no effective strategy");
    ok &= check(!r.detail.empty(), "invalid PE has detail");

    // Minimal MZ/PE header: enough to pass valid_pe(), then fail per strategy on null handle.
    std::vector<uint8_t> pe(0x400, 0);
    pe[0] = 'M'; pe[1] = 'Z';
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(pe.data());
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(pe.data() + 0x80);
    nt->Signature = 0x00004550; // "PE\0\0"

    Result r2 = inject(nullptr, 1234, pe, Options{});
    ok &= check(!r2.ok, "null handle fails cleanly");
    ok &= check(!r2.effective.has_value(), "failed Auto has no effective strategy");
    ok &= check(r2.attempts.size() == 2, "Auto tried exactly LoadLibrary and MapSectionImage");
    if (r2.attempts.size() == 2) {
        ok &= check(r2.attempts[0].strategy == Strategy::LoadLibrary, "Auto[0] is LoadLibrary");
        ok &= check(r2.attempts[1].strategy == Strategy::MapSectionImage, "Auto[1] is MapSectionImage");
    }

    // (c) --strategy=load-library with --no-unlink (unlink=false) does not fail due to flag and notes it.
    Options ll_opts{};
    ll_opts.strategy = Strategy::LoadLibrary;
    ll_opts.dll_path = L"C:\\ProgramData\\DisplayTuner\\display_core64.dll";
    ll_opts.unlink = false;
    Result r3 = inject(nullptr, 1234, pe, ll_opts);
    ok &= check(!r3.ok, "load-library with null handle fails on handle, not flag");
    ok &= check(r3.detail.find(L"ignorados") != std::wstring::npos, "load-library notes ignored flags in detail");

    std::printf("%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
