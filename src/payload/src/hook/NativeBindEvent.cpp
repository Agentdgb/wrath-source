// JVMTI NativeMethodBind interception — used to hook a handful of LWJGL GL
// entry points at bind time as a version-stable alternative to IAT patching.
// Not shipped in v0.1 (SwapBuffers via MinHook is sufficient for the overlay).

namespace dt::hook {

// void JNICALL nativeMethodBindThunk(jvmtiEnv*, JNIEnv*, jthread, jmethodID,
//                                    void* address, void** newAddressPtr) {
//     // Filter on org/lwjgl/opengl/GL*, override address for our small set.
// }

} // namespace dt::hook
