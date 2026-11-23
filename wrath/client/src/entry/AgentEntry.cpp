#include "entry/AgentEntry.h"

#include "entry/Bootstrap.h"
#include "platform/Win32Window.h"
#include "util/Log.h"
#include <windows.h>

namespace {

DWORD WINAPI agentBootstrapThread(LPVOID param) {
    JavaVM* vm = reinterpret_cast<JavaVM*>(param);
    // Wait until Minecraft's GLFW window is created so Fabric & Mixins finish
    // transforming classes on the main thread, avoiding classloader deadlocks.
    HWND wnd = vanta::platform::waitForGameWindow(60000, nullptr);
    if (!wnd) {
        Sleep(8000);
    } else {
        Sleep(500);
    }
    try {
        vanta::entry::bootstrap(vm, nullptr, vanta::entry::Door::AgentOnLoad);
    } catch (...) {
    }
    return 0;
}

} // namespace

extern "C" {

VANTA_AGENT_EXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* /*options*/, void* /*reserved*/) {
    vanta::entry::setAgentPresent(true);
    HANDLE t = CreateThread(nullptr, 0, &agentBootstrapThread, vm, 0, nullptr);
    if (t) CloseHandle(t);
    return JNI_OK;
}

VANTA_AGENT_EXPORT jint JNICALL Agent_OnAttach(JavaVM* vm, char* options, void* /*reserved*/) {
    vanta::entry::bootstrap(vm, options, vanta::entry::Door::AgentOnAttach);
    return JNI_OK;
}

VANTA_AGENT_EXPORT void JNICALL Agent_OnUnload(JavaVM* /*vm*/) {
    VLOG_INFO("boot", "Agent_OnUnload");
    vanta::entry::shutdown();
}

}  // extern C
