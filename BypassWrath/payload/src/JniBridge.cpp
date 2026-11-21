// JniBridge.cpp -- lets the JVM load bypass_payload.dll via System.load().
//
// JNI_OnLoad is the only symbol System.load requires. The payload itself is
// already started from DllMain(DLL_PROCESS_ATTACH), so this just returns the
// JNI version and gets out of the way.
//
// Declared with plain Win32 types so no JDK headers are needed to build the
// DLL. JNICALL is __stdcall on Windows; JNI_VERSION_1_8 is 0x00010008.

#include <windows.h>

extern "C" __declspec(dllexport)
int __stdcall JNI_OnLoad(void* /*vm*/, void* /*reserved*/) {
    return 0x00010008;
}
