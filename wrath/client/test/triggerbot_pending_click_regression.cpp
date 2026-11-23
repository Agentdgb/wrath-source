#include "../src/modules/combat/triggerbot/pending_click_logic.h"

#include <cstdio>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
    return 1; \
} } while (false)

using vanta::modules::triggerbot::pending_click::State;

int main()
{
    State state;
    constexpr std::uint64_t window = 500;
    const auto first = state.reserve(100, 0.95f, false);
    CHECK(first != 0);
    CHECK(state.pending(600, window));
    CHECK(state.reserve(600, 1.0f, true) == 0);
    CHECK(!state.pending(601, window)); // a lost click must not block forever

    const auto second = state.reserve(601, 0.8f, true);
    CHECK(second != 0 && second != first);
    state.cancel(first); // a late failure must not cancel the newer click
    const auto consumed = state.consume();
    CHECK(consumed.id == second && consumed.miss && consumed.cooldown_target == 0.8f);
    CHECK(!state.pending(602, window));

    const auto failed = state.reserve(700, 1.0f, true);
    state.cancel(failed); // failure to inject a miss leaves the trigger ready
    CHECK(!state.pending(700, window));
    CHECK(state.reserve(701, 1.0f, false) != 0);
    state.clear();
    CHECK(!state.pending(701, window));
    // Reproduce a render dispatch interleaved with the original attack.
    const auto hit = state.reserve(800, 1.0f, false);
    CHECK(hit != 0);
    state.begin_attack();
    CHECK(state.consume().id == hit);
    CHECK(state.pending(801, window));
    CHECK(state.reserve(801, 1.0f, false) == 0);
    CHECK(state.pending(2000, window)); // expiry cannot release an active call
    state.begin_attack();
    state.end_attack();
    CHECK(state.reserve(2000, 1.0f, false) == 0);
    state.end_attack();
    CHECK(!state.pending(2000, window));
    CHECK(state.reserve(2000, 1.0f, false) != 0);

}
