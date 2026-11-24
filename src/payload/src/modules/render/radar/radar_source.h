// radar_source.h — where the other players are, relative to where you are looking.
//
// Ported from Breeze's Radar (rip/breeze/client/Radar.java). Its maths is the
// part worth keeping and is reproduced exactly:
//
//   * positions are INTERPOLATED with the frame's tick progress, not sampled at
//     the tick — the difference between blips that glide and blips that step
//     twenty times a second,
//   * the bearing is -atan2(dx, dz) in degrees, which is Minecraft's own yaw
//     convention, so subtracting the viewer's yaw gives an angle where 0 is
//     straight ahead and positive is to the right,
//   * the fade is Breeze's: solid within ten blocks, linear to nothing at the
//     range setting. (Breeze divides by range-10 and so renders nothing at all
//     at its own minimum of 10; the divisor is floored at 1 here instead, which
//     is the only place the fade deliberately differs.)
//
// What is NOT ported is the presentation. Breeze draws its pointers around the
// crosshair with a pitch-driven tilt; this client draws a placeable panel, so
// Tilt and OnlyOffScreen have no meaning here and are gone rather than
// reinterpreted into something the source never said.
//
// Two more of its settings are absent for a duller reason: TeamColors needs a
// scoreboard-team read this client has no plumbing for, and Breeze's AntiBot
// filter belongs to its own AntiBot module. Neither is stubbed, so on a server
// that keeps NPCs in the player list they show up as ordinary blips.
//
// Same JNI contract as every other reader in this codebase: all jobject work
// happens inside one PushLocalFrame on the game thread, and what crosses out of
// it is plain floats.

#pragma once

#include <vector>

namespace vanta::modules::radar {

struct blip
{
    /* Degrees, relative to the viewer's facing: 0 is straight ahead, +90 is hard
       right. Already normalised to [-180, 180). */
    float bearing = 0.f;

    /* Blocks, three-dimensional — the same distance Breeze filters and fades on. */
    float distance = 0.f;

    /* Blocks, ignoring height: what the radial position on a top-down dial is
       actually measuring. */
    float ground = 0.f;

    /* Signed height difference in blocks. Breeze has no equivalent; a top-down
       radar that cannot tell you someone is on the roof above you is missing the
       one thing a top-down view cannot show by itself. */
    float height = 0.f;
};

/* @poll:
    rebuilds the blip list. Must be called on the game thread (the wglSwapBuffers
    detour) — the only thread allowed to touch the JVM here, and the same thread
    that draws.

    `range` is the cutoff in blocks; anyone further is not in the list at all. */
void poll(float range);

/* The last polled list, nearest first. */
const std::vector<blip>& latest();

/* @the viewer's own yaw, in Minecraft's convention.

   Published raw rather than pre-turned into a compass bearing, because that
   conversion is where the last version went wrong: it assumed yaw 0 was north.
   It is SOUTH -- 0 south, 90 west, 180 north, 270 east -- which is exactly the
   convention the bearings above are already in, since -atan2(dx, dz) reproduces
   it. One place to get that wrong is enough.

   The dial is view-up, so a cardinal point at absolute yaw A sits at relative
   bearing A - yaw, and the letters travel round the rim as you turn. Zero when
   the world has not been read yet. */
float viewer_yaw();

/* Forgets everything, so re-enabling the module does not flash the last world's
   players onto the panel. Named apart from radar::reset() in radar.h, which is
   the renderer's own reset and calls this one -- they share a namespace, and two
   functions of the same name in it would be one declaration with two bodies. */
void reset_source();

} // namespace vanta::modules::radar
