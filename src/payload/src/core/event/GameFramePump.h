#pragma once

namespace dt::core {

// Publishes GameLoopEvent once per presented frame.
//
// This is the client's ONLY tick source that actually fires today. The
// PreTickEvent path runs off HookBridge, which is not embedded yet, so modules
// listening on it never run. The wglSwapBuffers detour does fire — and in
// MC 1.21 the "Render thread" is the thread that runs the game loop and calls
// tick(), so a module driven from here is on the correct thread for JNI calls
// into the game.
//
// Call from the swap detour, once per frame, on the game's render thread.
// Faults inside listeners are contained: a broken module must not take the
// game down.
void pumpGameFrame();

} // namespace dt::core
