#pragma once

// Pump + state reset for SilentAim. The implementation lives in
// silentaim_tick.cpp; the module glue in SilentAimModule.cpp only calls these.

namespace dt::modules::silentaim_tick {

// One controller step, throttled internally to the 20 Hz world-tick rate.
// Called from GameLoopEvent, i.e. the wglSwapBuffers pump — which in MC 1.21
// is the game-loop thread, so every JNI call inside lands on the game thread.
void pump(float delta_ms);

// Drops the target, clears the integrals and the jitter, stops publishing a
// managed rotation, and unpublishes the target from the triggerbot hook.
void reset();
// The mouse-step estimator, shared with the action lease in silentaim.cpp.
//
// feed_lattice_observer() pushes one real-rotation sample (throttled to the
// 50 ms window the solver expects); observed_lattice_deg() is the consensus
// divisor, 0 until solved. It is a FALLBACK for when the sensitivity option
// cannot be read -- the lease never waits for it.
void  feed_lattice_observer(float real_yaw, float real_pitch);
float observed_lattice_deg();
// Snaps `target` onto real + k*lattice. Passes through when lattice is 0 or
// implausible, so an unknown lattice never blocks an action.
float snap_action_angle(float target, float real, float lattice);

} // namespace dt::modules::silentaim_tick
