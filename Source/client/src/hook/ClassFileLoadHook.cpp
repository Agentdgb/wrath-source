// The JVMTI ClassFileLoadHook C thunk that routes into HookManager lives inside
// hook/HookManager.cpp so it can share the singleton's private state. This TU
// exists so hook/ClassFileLoadHook.cpp remains a discoverable name in the
// tree and can be extended with diagnostics (per-class transform log,
// symbolication) later.

