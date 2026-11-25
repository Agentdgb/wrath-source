#include "entry/Bootstrap.h"

#include "core/HeartbeatWatchdog.h"
#include "core/config/ConfigManager.h"
#include "core/config/ProfileStorage.h"
#include "core/UnloadSafety.h"
#include "core/keybind/KeybindManager.h"
#include "core/module/ModuleManager.h"
#include "core/profile/PlayerProfile.h"
#include "hook/HookManager.h"
#include "hook/PacketEvents.h"
#include "jvm/JvmContext.h"
#include "jvm/JniHelper.h"
#include "jvm/VersionDetect.h"
#include "mappings/MappingRegistry.h"
#include "modules/combat/autototem/autototem.h"
#include "modules/combat/silentaim/silentaim.h"
#include "modules/combat/triggerbot/triggerbot.h"
#include "modules/visuals/ports/VisualPorts.h"
#include "platform/Win32Console.h"
#include "platform/Win32Process.h"
#include "platform/Win32Window.h"
#include "platform/ThreadUtil.h"
#include "platform/ModuleRelease.h"
#include "platform/ManualUnmap.h"
#include "util/AtomicOnce.h"
#include "util/Log.h"
#include "vgui/core/logger.hh"
#include "vgui/vgui.h"

#include "shared/stealth/StealthDefs.h"
#include "shared/diagnostics/SessionDiagnostics.h"
#include <jnihook.h>
#include <windows.h>
#include <shlobj.h>
#include <vector>

namespace dt::entry {

std::atomic<bool> g_agentPresent{false};

namespace {

AtomicOnce g_once;
std::atomic<UnloadState> g_unloadState{UnloadState::Idle};
std::atomic<DWORD> g_unloadError{0};
std::atomic<HWND> g_notificationWindow{nullptr};

// User-facing failures are independent of the optional diagnostic logger.
// Win32 calls and atomics only: this is also callable by the bare reaper.
void reportUnloadFailure(UnloadState state, const wchar_t* reason, DWORD error = 0) {
    g_unloadError.store(error);
    g_unloadState.store(state);
    const HWND owner = g_notificationWindow.load();
    MessageBoxW(IsWindow(owner) ? owner : nullptr,
        reason ? reason : L"La descarga no se completo. El DLL permanece cargado.",
        L"Wrath - descarga incompleta", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

// Wrath Loader watches this per-PID named object (blended as a standard Windows
// Feature Configuration / WIL staging object) to confirm that the payload
// reached Dt's bootstrap entry point.
std::atomic<HANDLE> g_sentinelMutex{nullptr};

std::wstring sentinelMutexName(DWORD pid) {
    return wrath::bypass::sentinelMutexNameForPid(pid);
}

// One teardown only, whichever path reaches it first (F8 poll, or an external
// requestFullUnload).
std::atomic<bool> g_unloading{false};

void unloadOnThisThread();   // defined below; returns once the reaper is armed
void reapAndFree();          // bare thread: waits for the image to empty, frees

// Non-Fabric loaders cannot be hooked (the shipped mapping tables are
// intermediary-only). We say so and carry on WITHOUT the Java hook layer —
// the GUI and the config still run, so the user gets a window and a reason
// instead of a process that silently does nothing.
void warnNonFabric(dt::jvm::Loader detected) {
    const char* name = dt::jvm::loaderName(detected);
    VLOG_ERR("boot",
        "Java hooks disabled: WrathClient ships Fabric intermediary mappings only. "
        "Detected loader: {}. The GUI still runs; game-side modules stay inert.", name);
}

// vgui talks to slog. Route its emissions into our wrath.log so overlay
// creation, D3D11 init, font loading, all show up in the same trace as boot.
// EVERY level maps to Info so slog::debug ("created font: X") is not filtered
// out — that granularity is what pinpoints crashes inside vgui init.
void routeSlogIntoLog(int level, const char* message) {
    dt::log::Level lvl = dt::log::Level::Info;
    if (level == 2) lvl = dt::log::Level::Warn;
    if (level == 3) lvl = dt::log::Level::Error;
    dt::log::write(lvl, "vgui", message ? std::string(message) : std::string());
}

#ifdef _DEBUG
LPTOP_LEVEL_EXCEPTION_FILTER g_prevExceptionFilter = nullptr;

// Last-chance crash reporter (Debug only so Release never hooks kernel32 top-level
// exception handling or leaves a dangling filter pointer after F8 unload).
LONG WINAPI dtUnhandledFilter(EXCEPTION_POINTERS* ep) {
    char msg[256];
    std::snprintf(msg, sizeof(msg),
        "UNHANDLED EXCEPTION code=0x%08lX addr=%p flags=0x%08lX",
        ep->ExceptionRecord->ExceptionCode,
        ep->ExceptionRecord->ExceptionAddress,
        ep->ExceptionRecord->ExceptionFlags);
    dt::log::write(dt::log::Level::Fatal, "crash", msg);
    dt::log::shutdown();   // force flush
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// SEH-wrapped body. A C++ try/catch does NOT catch access violations, stack
// overflows, or other structured exceptions — and a fault inside MinHook's
// trampoline writer or a driver's context creation is exactly that. MSVC
// forbids __try in any function containing objects with destructors, so the
// SEH wrapper and the C++ callee are physically separated.
void guiCallCppSide(HWND wnd, bool* ok) { *ok = dt::vgui::start(wnd); }

DWORD guiArmSEH(HWND wnd, bool* ok) {
    __try {
        guiCallCppSide(wnd, ok);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

// SEH-wrapped JVM setup. jvm.dll's JNI_GetCreatedJavaVMs / AttachCurrentThreadAsDaemon
// can fault in-process if the VM is mid-init, mid-shutdown, or in a state we
// haven't authorised. Any such fault must NOT take down the whole client —
// we log it, leave the Java hook layer inert, and let the GUI keep running.
void jvmCallCppSide() {
    dt::jvm::initFromRunningProcess();
    if (dt::jvm::ctx().vm.load()) {
        dt::jvm::requestCapabilities();
    }
}

DWORD jvmBringUpSEH() {
    __try { jvmCallCppSide(); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

void jvmCallDetect(JNIEnv* env, dt::jvm::RuntimeInfo* out) {
    *out = dt::jvm::detectRuntime(env);
}

DWORD jvmDetectSEH(JNIEnv* env, dt::jvm::RuntimeInfo* out) {
    __try { jvmCallDetect(env, out); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

// The client draws inside Minecraft's own frame, so there is no GUI thread and
// no window of ours to pump: arming the overlay is a one-shot that installs a
// wglSwapBuffers detour and hands rendering to the game's render thread.
//
// It still runs off the bootstrap thread, because both preconditions — the
// GLFW window existing and opengl32.dll being loaded — can be seconds away
// when a launcher injects during its splash screen. Blocking bootstrap on that
// would delay (or, under AgentOnLoad, deadlock) the JVM bring-up below.
void armOverlayAsync() {
    dt::platform::spawnNamed("Render Thread", []{
        slog::log::set_external_sink(&routeSlogIntoLog);

        HWND wnd = dt::platform::waitForGameWindow(20000, &g_unloading);
        if (!wnd) {
            VLOG_ERR("gui", "no game window after 20s — overlay not armed");
            return;
        }

        {
            wchar_t cls[64] = {};
            GetClassNameW(wnd, cls, 64);
            char narrow[64] = {};
            WideCharToMultiByte(CP_UTF8, 0, cls, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
            VLOG_INFO("gui", "game window {} class='{}'",
                      reinterpret_cast<std::uintptr_t>(wnd), std::string(narrow));
        }

        // opengl32 can lag the window when the launcher boots through
        // Swing/JavaFX and only loads LWJGL once the game itself starts.
        HMODULE gl = nullptr;
        for (int i = 0; i < 25 && !gl; ++i) {
            if (g_unloading.load()) return;
            gl = GetModuleHandleA("opengl32.dll");
            if (!gl) Sleep(200);
        }
        if (!gl) {
            VLOG_ERR("gui", "opengl32.dll never loaded — overlay not armed "
                            "(a Vulkan-only renderer is not supported)");
            return;
        }

        // Retry: the window class can still be in transition between LWJGL and
        // GLFW30, and a hook attempt landing in that gap fails harmlessly.
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (g_unloading.load()) return;
            if (attempt) Sleep(500);

            bool  ok   = false;
            DWORD code;
            {
                std::lock_guard state(dt::clientStateMutex());
                if (g_unloading.load()) return;
                code = guiArmSEH(wnd, &ok);
            }

            if (code != 0) {
                char hex[16]; std::snprintf(hex, sizeof(hex), "%08lX", code);
                VLOG_ERR("gui", "overlay arm faulted (SEH 0x{}) — client stays inert", std::string(hex));
                return;
            }
            if (ok) {
                VLOG_INFO("gui", "overlay armed; it initialises on the next game frame");
                return;
            }
            VLOG_WARN("gui", "overlay arm attempt {} failed, retrying", attempt + 1);
        }

        VLOG_ERR("gui", "overlay could not be armed after 5 attempts");
    });
}

void bootstrapUnlocked(JavaVM* vm, const char* options, Door door) {
    wrath::diagnostics::recordModule(dt::platform::currentModule());
    const std::wstring mtxName = sentinelMutexName(GetCurrentProcessId());
    HANDLE sentinel = CreateMutexW(nullptr, FALSE, mtxName.c_str());
    g_sentinelMutex.store(sentinel, std::memory_order_release);

    // 1) Logging first — everything else logs its progress.
    // logDir() returns the directory containing WrathPayload.dll (falls back to
    // %APPDATA%\WrathClient if that directory is unwritable). Everything ALSO
    // goes through OutputDebugStringA, so DbgView.exe shows the boot even when
    // the file could not be opened.
#ifdef _DEBUG
    auto logDir = dt::platform::logDir();
    dt::log::init(dt::log::Level::Info, std::filesystem::path(logDir) / L"wrath.log");
    g_prevExceptionFilter = SetUnhandledExceptionFilter(&dtUnhandledFilter);
#endif
    VLOG_INFO("boot", "WrathClient booting (door={})",
              door == Door::AgentOnLoad   ? "AgentOnLoad" :
              door == Door::AgentOnAttach ? "AgentOnAttach" : "NativeInject");
    if (options && *options) VLOG_INFO("boot", "options='{}'", options);

    // 2) Modules + config + keybinds. All pure C++ — no JNI is required to
    // build the module registry, so this happens BEFORE any JVM work and the
    // GUI is guaranteed a populated module list.
    dt::ModuleManager::get().initAll();
    dt::ConfigManager::get().init(dt::platform::appDataDir());
    if (!dt::ConfigManager::get().isInitialized()) {
        VLOG_ERR("cfg", "No safe profile directory available");
    }
    dt::KeybindManager::get().init();
    dt::ConfigManager::get().loadProfile(dt::ConfigManager::get().currentProfile());

    // Hotkeys are processed directly in the window procedure (gl_hook.cc) with 0 ms latency,
    // avoiding dedicated polling threads and GetAsyncKeyState loops.
    auto& binds = dt::KeybindManager::get();
    dt::vgui::setHotkeys(binds.guiToggleKey(), binds.panicKey());

    // 3) Overlay. Deliberately armed BEFORE the JVM work: a failure to find
    // the VM, detect the version, or resolve mappings must never leave the
    // user with no interface and no explanation.
    VLOG_INFO("boot", "step: arming in-game overlay");
    armOverlayAsync();

    // 4) JVM context. WRAPPED IN SEH: jvm.dll may fault if the VM is mid-init.
    // Any such fault is turned into a log line, the GUI keeps running.
    VLOG_INFO("boot", "step: bringing up JVM (SEH-guarded)");
    if (vm) {
        dt::jvm::init(vm);
    } else {
        DWORD ec = jvmBringUpSEH();
        if (ec != 0) {
            char hex[16]; std::snprintf(hex, sizeof(hex), "%08lX", ec);
            VLOG_ERR("boot", "JVM bring-up faulted (SEH 0x{}) — Java hooks disabled, GUI OK", std::string(hex));
            return;
        }
    }

    if (!dt::jvm::ctx().vm.load()) {
        VLOG_ERR("boot", "No JavaVM in this process — Java hooks disabled, GUI OK");
        return;
    }

    VLOG_INFO("boot", "step: attaching JNIEnv");
    JNIEnv* env = dt::jvm::thisThreadEnv();
    if (!env) {
        VLOG_ERR("boot", "No JNIEnv — Java hooks disabled, GUI OK");
        return;
    }

    // 5) Version + loader detect (also SEH-guarded — SharedConstants reflection
    // can crash in an unstable VM).
    VLOG_INFO("boot", "step: detecting loader + MC version");
    dt::jvm::RuntimeInfo ri;
    DWORD ec = jvmDetectSEH(env, &ri);
    if (ec != 0) {
        char hex[16]; std::snprintf(hex, sizeof(hex), "%08lX", ec);
        VLOG_ERR("boot", "loader detect faulted (SEH 0x{}) — Java hooks disabled, GUI OK", std::string(hex));
        return;
    }

    if (ri.loader != dt::jvm::Loader::Fabric) {
        warnNonFabric(ri.loader);
        return;
    }

    VLOG_INFO("boot", "step: binding MappingRegistry to MC {}", ri.mcVersion);
    dt::mappings::MappingRegistry::get().init(ri.mcVersion);

    // 6) Hooks (Java-side via JVMTI).
    VLOG_INFO("boot", "step: installing JVMTI hooks");
    dt::hook::HookManager::get().install(dt::jvm::ctx().jvmti.load(), env);

    VLOG_INFO("boot", "Bootstrap complete (transformed={} classes)",
              dt::hook::HookManager::get().transformedClassCount());

    // Fetch account profile & avatar asynchronously once JVM & mappings are active
    dt::profile::start();

    // Iniciar verificación continua de latido con el servidor
    dt::sync::HeartbeatWatchdog::get().start();
}

} // namespace

void bootstrap(JavaVM* vm, const char* options, Door door) {
    g_once.run([&]{ bootstrapUnlocked(vm, options, door); });
}
bool bootstrapFinished() noexcept { return g_once.done() || g_once.failed(); }
UnloadState unloadState() noexcept { return g_unloadState.load(); }
unsigned long unloadError() noexcept { return g_unloadError.load(); }

void shutdown() {
    // Each step is announced BEFORE it runs. If teardown ever hangs or faults,
    // the last line in the log names the step that did it — without this the
    // log simply stops and every step is equally suspect.
    VLOG_INFO("boot", "Shutdown sequence beginning");

#ifdef _DEBUG
    SetUnhandledExceptionFilter(g_prevExceptionFilter);
    g_prevExceptionFilter = nullptr;
#endif

    if (HANDLE sentinel = g_sentinelMutex.exchange(nullptr, std::memory_order_acq_rel)) {
        CloseHandle(sentinel);
    }

    // Detener de inmediato el supervisor de latidos
    dt::sync::HeartbeatWatchdog::get().stop();

    // The overlay is torn down by unloadOnThisThread before it gets here (it
    // has to be — see the comment there). Calling stop() again is a no-op.
    VLOG_INFO("boot", "  shutdown: profile"); dt::profile::stop();
    if (!dt::hook::packets::shutdown()) return;
    VLOG_INFO("boot", "  shutdown: vgui");     dt::vgui::stop();
    VLOG_INFO("boot", "  shutdown: keybinds"); dt::KeybindManager::get().shutdown();
    VLOG_INFO("boot", "  shutdown: config");
    dt::ConfigManager::get().shutdown();

    // Raise shutdown flags first so any in-flight JNIHook callback immediately
    // bails out to the original method before caches and hooks are torn down.
    dt::modules::autototem::g_shutting_down.store(true, std::memory_order_release);
    dt::modules::triggerbot::g_shutting_down.store(true, std::memory_order_release);
    dt::modules::silentaim::g_shutting_down.store(true, std::memory_order_release);

    VLOG_INFO("boot", "  shutdown: modules");  dt::ModuleManager::get().shutdownAll();

    VLOG_INFO("boot", "  shutdown: autototem");
    dt::modules::autototem::shutdown_cache();

    VLOG_INFO("boot", "  shutdown: tt");
    dt::modules::triggerbot::shutdown_cache();

    VLOG_INFO("boot", "  shutdown: sa");
    dt::modules::silentaim::shutdown_cache();

    VLOG_INFO("boot", "  shutdown: hooks");
    dt::hook::HookManager::get().uninstall();
    jnihook_result_t jhRes = JNIHook_Shutdown();
    for (int retry = 0; retry < 3 && jhRes != JNIHOOK_OK && jhRes != JNIHOOK_ERR_MUST_POSSESS_CAPABILITY; ++retry) {
        Sleep(50);
        jhRes = JNIHook_Shutdown();
    }
    if (jhRes != JNIHOOK_OK) { dt::blockUnload(); return; }
    dt::modules::visualports::shutdownCache();

    // Profile/artwork workers must finish before the VM context and its
    // shared lookup references are retired. This shutdown worker is counted.
    for (int attempt = 0; attempt < 200 && dt::platform::liveThreadCount() > 1; ++attempt) Sleep(10);
    if (dt::platform::liveThreadCount() > 1) { dt::blockUnload(); return; }
    dt::jvm::invalidateCaches();
    dt::jvm::detachThisThreadIfAttached();
    VLOG_INFO("boot", "  shutdown: jvm");      dt::jvm::shutdown();

    // Erase all diagnostic artefacts (load/module/unload JSON files) written
    // during this session. Done BEFORE closing the log so any write failure
    // can still be logged, but after all subsystems that produce these files
    // have been stopped. Failure to purge is non-fatal: the shutdown proceeds.
    VLOG_INFO("boot", "  shutdown: purge diagnostics");
    wrath::diagnostics::purgeSessionRecords(GetCurrentProcessId());

    VLOG_INFO("boot", "  shutdown: log");
    dt::log::shutdown();
    dt::platform::closeConsole();
}

namespace {

void unloadOnThisThread() {
    if (g_unloading.exchange(true)) {
        /* Another path is already tearing down. RETURN, do not park: parking
           would hold this thread's CRT module reference for ever, and the
           reaper waits for exactly that count to reach zero. */
        return;
    }

    VLOG_WARN("boot", "full unload requested");
    g_unloadState.store(UnloadState::Stopping);
    for (int attempt = 0; attempt < 100 && !bootstrapFinished(); ++attempt) Sleep(50);
    if (!bootstrapFinished()) {
        g_unloading.store(false);
        reportUnloadFailure(UnloadState::Failed,
            L"La inicializacion no termino. No se inicio la descarga y el DLL sigue cargado.");
        return;
    }
    dt::EventBus::get().stop();
    dt::profile::stop();
    dt::modules::autototem::g_shutting_down.store(true);
    dt::modules::triggerbot::g_shutting_down.store(true);
    dt::modules::silentaim::g_shutting_down.store(true);

    /* Tear the overlay down FIRST, and synchronously. This is a hard
       precondition, not a courtesy: the render thread executes our detour, our
       GL objects live in a context only that thread may touch, and the
       cursor_guard detours on user32 stay reachable until they are removed.
       Freeing the image while any of that is live turns the game's next
       SwapBuffers or SetCursorPos into a jump into unmapped memory.

       vgui::stop() raises a flag the render thread acts on, waits for its
       acknowledgement, then drains the detour before removing it. */
    dt::vgui::stop();

    if (dt::vgui::isRunning() || !dt::safeToUnload()) {
        /* Refuse to free the image while callbacks may still reference it.
           Keep their caches alive and report the incomplete shutdown. */
        VLOG_ERR("boot",
            "the render thread never acknowledged shutdown — leaving the DLL "
            "resident. The client is stopped, but the image cannot be freed safely.");
        // Do not destroy module caches while callbacks can still use them.
        // Return normally so this worker does not stay parked in the image.
        g_unloading.store(false);
        reportUnloadFailure(UnloadState::StoppedResident,
            L"La descarga no se completo: el render o los hooks no confirmaron un cierre seguro.\n"
            L"El DLL permanece cargado para no liberar recursos todavia en uso.");
        return;
    }

    // Resolved BEFORE shutdown() runs, because shutdown() closes the log and a
    // failure after that point would be invisible.
    HMODULE self = dt::platform::currentModule();
    if (!self) {
        VLOG_ERR("boot", "no module handle — cannot unload; the DLL stays resident");
        g_unloading.store(false);
        reportUnloadFailure(UnloadState::Failed,
            L"No se pudo identificar el modulo. El DLL permanece cargado.");
        return;
    }
    VLOG_INFO("boot", "will free module at {}", reinterpret_cast<std::uintptr_t>(self));

    shutdown();

    if (!dt::safeToUnload()) {
        reportUnloadFailure(UnloadState::StoppedResident,
            L"El cierre de callbacks o hilos no termino correctamente. El DLL permanece cargado.");
        return;
    }

#ifdef _DEBUG
    OutputDebugStringA("[wrath] teardown done, handing off to the reaper\n");
#endif
    if (!dt::platform::spawnReaper(&reapAndFree)) {
        reportUnloadFailure(UnloadState::StoppedResident,
            L"El cliente termino su cierre, pero no se pudo crear el hilo de liberacion.\n"
            L"El DLL permanece cargado.", GetLastError());
        return;
    }
    g_unloadState.store(UnloadState::ReleaseRequested);
}

// Runs on the bare thread from spawnReaper, using Win32 calls and atomics:
// this code outlives every other thread in the image on purpose.
void reapAndFree() {
    HMODULE self = dt::platform::currentModule();
    if (!self) {
        reportUnloadFailure(UnloadState::Failed,
            L"El hilo de liberacion no encontro el modulo. El DLL permanece cargado.");
#ifdef _DEBUG
        OutputDebugStringA("[wrath] no module handle; staying resident\n");
#endif
        return;
    }

    // Every spawnNamed thread must have returned, releasing its CRT reference.
    for (int i = 0; i < 1000 && dt::platform::liveThreadCount() > 0; ++i)
        Sleep(10);

    if (dt::platform::liveThreadCount() > 0) {
        reportUnloadFailure(UnloadState::StoppedResident,
            L"Todavia hay hilos activos. Se cancelo la liberacion del DLL.");
#ifdef _DEBUG
        OutputDebugStringA("[wrath] threads still inside the image; staying resident\n");
#endif
        return;
    }

    DWORD releaseError = ERROR_SUCCESS;
    if (dt::platform::isRegisteredDll(self, releaseError)) {
        // ── Normal path: image in PEB loader list ─────────────────────────
        // FreeLibraryAndExitThread releases the module reference and kills
        // this thread atomically: no instant where this thread executes
        // freed code.
        Sleep(250);
#ifdef _DEBUG
        OutputDebugStringA("[wrath] unloading WrathPayload.dll (LoadLibrary path)\n");
#endif
        FreeLibraryAndExitThread(self, 0);
        // unreachable
    }

    // ── Manual-map path: image NOT in PEB (TxF SEC_IMAGE or classic MM) ──
    // FreeLibrary has no effect because the module has no PEB Ldr entry.
    // NtUnmapViewOfSection releases the mapped pages directly.
    //
    // CRITICAL: after NtUnmapViewOfSection the image bytes are gone. We must
    // NOT execute any code that lives in the image after that point. The
    // strategy below is:
    //   1. Resolve ExitThread from kernel32 (a system DLL that stays mapped).
    //   2. Call unmapSelfRegion — this frees the image (including this code).
    //   3. Jump directly to ExitThread via the pre-resolved pointer.
    // Between step 2 and 3 no instruction from the image runs; the return
    // address from unmapSelfRegion lands on a ret in kernel32's stack frame,
    // not in the image. unmapSelfRegion is `inline` in the header, so its
    // body is compiled into the caller's stack frame on the reaper thread
    // (which is NOT inside the image — it is a raw OS thread).
    //
    // Ruling: spawnReaper uses CreateThread (bare Win32), not _beginthreadex,
    // so this thread holds no CRT module reference. The inline unmapSelfRegion
    // call therefore runs from the thread's own stack, not from image pages
    // that get freed mid-execution.

#ifdef _DEBUG
    OutputDebugStringA("[wrath] unloading WrathPayload.dll (manual-map path via stub reaper)\n");
#endif

    Sleep(250);

    // Update state before memory is destroyed
    g_unloadState.store(UnloadState::ReleaseRequested);

    // Isolated stub executes VirtualFree/NtUnmapViewOfSection and tail-jumps to ExitThread(0)
    dt::platform::executeManualUnloadAndExit(self);
}

} // namespace

void requestFullUnload() {
    requestFullUnload(false);
}

void requestFullUnload(bool /*purgeConfig*/) {
    g_notificationWindow.store(dt::platform::findGameWindow());
    // Compatibility overload: Unload always preserves profiles and user data.
    wrath::diagnostics::recordUnloadRequest();
    // Callers reach this from threads that must survive (a JNI callback, the
    // render thread), so the teardown runs on a thread of its own — the one
    // that will eventually free the image and exit with it.
    try {
        if (!dt::platform::spawnNamed("GC Finalizer", [] {
            try { unloadOnThisThread(); }
            catch (...) {
                dt::blockUnload();
                reportUnloadFailure(UnloadState::StoppedResident,
                    L"El cierre encontro un error inesperado. El DLL permanece cargado.");
            }
        })) {
            reportUnloadFailure(UnloadState::Failed,
                L"No se pudo iniciar el hilo de descarga. El cliente sigue cargado.", GetLastError());
        }
    } catch (...) {
        reportUnloadFailure(UnloadState::Failed,
            L"No se pudo preparar la descarga. El cliente sigue cargado.", ERROR_NOT_ENOUGH_MEMORY);
    }
}

} // namespace dt::entry

