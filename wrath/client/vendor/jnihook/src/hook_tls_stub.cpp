// hook_tls_stub.cpp — stub for the jnihook TLS-wire hook.
//
// jnihook.cpp calls Hook::ensure_tls_for_this_thread() at the top of its
// ClassFileLoadHook to fix up the PE TLS block on JVM threads that were
// spawned after injection (needed on LoadLibrary paths where the injector
// didn't wire TLS for those threads — without it, first STL access on
// that thread crashes with AV on gs:[58] + tls_index*8).
//
// Vanta ships primarily as a JVMTI agent (-agentpath / Attach API), in
// which case the JVM sets TLS up for us before any callback fires — so
// the stub can be a no-op. On the LoadLibrary path Vanta uses its own
// TLS wire-up inside vanta::jvm::init(), well before jnihook attaches
// anything. Either way the fix-up is unnecessary here, so the symbol
// just resolves to nothing.
namespace Hook {

void ensure_tls_for_this_thread() {}

} // namespace Hook
