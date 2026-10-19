#include "core/event/GameFramePump.h"

#include "core/event/EventBus.h"
#include "core/event/Events.h"
#include "jvm/JvmContext.h"
#include "hook/PacketEvents.h"
#include "util/Log.h"

#include <windows.h>

namespace vanta::core {

namespace {

/* QueryPerformanceCounter, not GetTickCount64. The tick counter moves in
   10-16 ms steps, so at 120-240 fps most frames read a delta of 0 and every
   second or third one reads 16 ms; a module integrating motion with that
   (Freecam) advanced in bursts. QPC is sub-microsecond on every Windows this
   client runs on, and its frequency is fixed for the life of the process. */
LARGE_INTEGER g_qpc_freq{};
long long     g_last_qpc = 0;

// MSVC forbids __try in a function holding anything with a destructor, so the
// C++ body and the SEH guard are separate functions.
void publish(float deltaMs) {
    events::GameLoopEvent e{};
    e.deltaMs = deltaMs;
    EventBus::get().post(e);
}

DWORD publishSEH(float deltaMs) {
    __try { publish(deltaMs); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

} // namespace

void pumpGameFrame() {
    // No VM, nothing for a module to talk to.
    if (!vanta::jvm::ctx().vm.load()) return;
    vanta::hook::packets::install();

    if (g_qpc_freq.QuadPart == 0) QueryPerformanceFrequency(&g_qpc_freq);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    float delta = 0.f;   // first frame: nothing to measure against
    if (g_last_qpc != 0 && g_qpc_freq.QuadPart > 0)
        delta = static_cast<float>(static_cast<double>(now.QuadPart - g_last_qpc) * 1000.0
                                   / static_cast<double>(g_qpc_freq.QuadPart));
    g_last_qpc = now.QuadPart;

    // A frame that took a third of a second means the game stalled (chunk
    // load, GC pause). Handing that through would make every time-based
    // module jump; clamp it the same way the gui clamps its own delta.
    if (delta > 333.f) delta = 333.f;

    // Proof of life, once. If this line is absent from the log, no module can
    // possibly be running and the problem is upstream of every one of them.
    static bool announced = false;
    if (!announced) {
        announced = true;
        VLOG_INFO("tick", "game frame pump live — modules are being driven");
    }

    const DWORD fault = publishSEH(delta);
    if (fault != 0) {
        // Once. This runs every frame; a repeating fault would otherwise
        // write megabytes before anyone looked at the log.
        static bool warned = false;
        if (!warned) {
            warned = true;
            char hex[16];
            std::snprintf(hex, sizeof(hex), "%08lX", fault);
            VLOG_ERR("tick", "a module faulted on the game frame (SEH 0x{}) — "
                             "further occurrences are not logged", std::string(hex));
        }
    }
}

} // namespace vanta::core
