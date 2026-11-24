#include "modules/combat/antibot/activity_logic.h"
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main() {
    using vanta::modules::antibot::Activity;
    Activity input;
    constexpr auto game = 123u;
    CHECK(!input.active(100, game, 300));
    input.note(100, game, false); // trigger's injected mouse click
    CHECK(!input.active(100, game, 300));
    input.note(200, game, true); // physical mouse delta
    CHECK(input.active(499, game, 300));
    CHECK(!input.active(500, game, 300));
    input.note(501, game, false); // automation cannot extend the timeout
    CHECK(!input.active(501, game, 300));
    CHECK(!input.active(100, game, 300)); // invalid/backwards timestamp
    input.key(600, game, 1u, true, true); // intentionally holding W
    CHECK(input.active(5000, game, 300));
    input.key(5001, game, 1u, false, false); // ignore synthetic key releases
    CHECK(input.active(6000, game, 300));
    input.key(6001, game, 1u, false, true);
    CHECK(input.active(6300, game, 300));
    CHECK(!input.active(6301, game, 300));
    input.note(7000, game, true);
    CHECK(!input.active(7001, 456u, 300)); // another application is foreground
    input.reset(); // focus loss or menu opened
    CHECK(!input.active(7001, game, 300));
    input.note(8000, game, true);
    CHECK(input.active(8000, game, 300)); // immediate resume, no extra delay
    CHECK(!input.active(8300, game, 300)); // damage/knockback supplies no input
    std::puts("AntiBot physical input, idle timeout, held movement, injection and focus: OK");
}
