// Verifies the injection API contract without touching a real process:
//  - all four strategies parse and round-trip through strategy_name()
//  - inject() with an invalid PE returns ok=false with a detail (no crash)
//  - inject() with a valid PE header but a null handle reports failures per
//    strategy without throwing (stomp size failures are reported, not fatal)
#include "inject/Injector.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace wrath::inject;

static bool check(bool cond, const char* what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    return cond;
}

int main() {
    bool ok = true;

    const char* names[] = { "map-private", "map-section-image", "map-stomp", "run-thread", "run-hijack" };
    for (const char* n : names) {
        Strategy s{};
        bool parsed = parse_strategy(n, &s);
        ok &= check(parsed, n);
        if (parsed) ok &= check(std::string(strategy_name(s)) == n, "round-trip");
    }

    std::vector<uint8_t> bad(64, 0);
    Result r = inject(nullptr, 1234, bad, Options{});
    ok &= check(!r.ok, "invalid PE rejected");
    ok &= check(!r.detail.empty(), "invalid PE has detail");

    // A minimal MZ/PE header: enough to pass valid_pe(), then fail per strategy
    // on the null handle. Each attempt must produce a detail, not a crash.
    std::vector<uint8_t> pe(0x400, 0);
    pe[0] = 'M'; pe[1] = 'Z';
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(pe.data());
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(pe.data() + 0x80);
    nt->Signature = 0x00004550; // "PE\0\0"

    Result r2 = inject(nullptr, 1234, pe, Options{});
    ok &= check(!r2.ok, "null handle fails cleanly");
    ok &= check(r2.attempts.size() >= 1, "attempts recorded");

    std::printf("%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
