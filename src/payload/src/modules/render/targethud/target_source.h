// target_source.h — who the player is fighting, reduced to plain data.
//
// Nothing in this client published that before. SilentAim and the TriggerBot
// both know their target, but each keeps it as a bare jobject with no getter,
// and SilentAim deliberately withholds its channel until the rotation has been
// locked on for two ticks — the exact window a HUD most wants to draw. So the
// Target HUD does its own read rather than borrowing theirs, and does not care
// whether any combat module is enabled.
//
// The contract every JNI reader in this codebase follows, and this one too: all
// jobject work happens inside one PushLocalFrame on the game thread, and what
// crosses out of it is floats and a string. A jobject must never reach the draw
// pass — it may be collected or moved between the two, and the draw pass has no
// env to validate it with.

#pragma once

#include <string>

namespace vanta::modules::targethud {

struct target_snapshot
{
    bool valid = false;

    std::string name{};

    float health = 0.f;
    float max_health = 20.f;

    /* Absolute world position of the target's feet, INTERPOLATED to this frame
       rather than sampled at the tick. The difference is the whole of "does the
       HUD glide along with them or step 20 times a second". */
    double x = 0.0, y = 0.0, z = 0.0;

    /* eye height above `y`, so a follow HUD can sit above the head instead of
       inside the chest */
    float height = 1.8f;

    float distance = 0.f;
};

/* @poll:
    reads the current target and updates the snapshot. Must be called on the
    game thread (the wglSwapBuffers detour) — that is the only thread allowed to
    touch the JVM here, and it is the same thread that draws.

    @what counts as a target: someone you HIT, not someone you looked at.

    A crosshair-driven HUD flickers on for every player you sweep past, which in
    a crowded lobby is constantly and tells you nothing. A swing that lands on
    somebody is an unambiguous statement that they are who you are fighting.

    Once locked, the target is TRACKED rather than re-picked: the entity is found
    again in the world every frame by name, so its health and position keep
    updating after your crosshair has left it — which is most of a melee, and is
    the whole reason the panel is worth having.

    `linger` is how long the lock survives without a fresh hit.

    `range` is how far away it survives, in blocks. Zero or less disables the
    cut. Without one the lock is bounded only by time, so a target who runs keeps
    the panel on screen at any distance — and a fixed-size panel pinned beside a
    player who is now a few pixels tall is what reads as the panel growing. */
void poll(float delta, float linger, float range);

/* @latest:
    the last polled target. `valid` is false when there is nothing to draw. */
const target_snapshot& latest();

/* @presence:
    0..1, rising while a target is held and falling once it is lost. Drives the
    HUD's own fade, so the panel eases out instead of vanishing on the frame the
    linger expires. */
float presence();

/* Forgets everything. Called when the module is disabled so re-enabling it does
   not flash the previous fight's target. */
void reset();

} // namespace vanta::modules::targethud
