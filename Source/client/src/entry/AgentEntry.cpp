#include "entry/AgentEntry.h"

#include "entry/Bootstrap.h"
#include "util/Log.h"

extern "C" {

VANTA_AGENT_EXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void* /*reserved*/) {
    vanta::entry::bootstrap(vm, options, vanta::entry::Door::AgentOnLoad);
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
