// chams_crash.h — say WHERE it died, instead of just stopping.
//
// Why this file exists
// --------------------
// A 1.21.4 client crashed on inject and the desktop log ended mid-sentence at
// "gl layer armed". That tail is worth almost nothing: it proves the hooks went
// in and proves nothing about what ran afterwards. Every diagnosis from a log
// that simply STOPS is a guess dressed up as a finding.
//
// So: a vectored exception handler that runs before the JVM's own, records the
// faulting address, the module that owns it, and the last chams breadcrumb, and
// then gets out of the way.
//
// What it deliberately does NOT do
// --------------------------------
// It never handles anything. EXCEPTION_CONTINUE_SEARCH, always. The JVM raises
// access violations as a matter of course — implicit null checks and safepoint
// polling are built on them — so a handler that swallowed them, or even merely
// logged them all, would both spam the log and slow the VM down. The filter
// below is what keeps it quiet: a fault is only reported when it is plausibly
// OURS (inside this DLL, inside a chams detour, or inside the GL driver while a
// chams detour is on the stack).

#pragma once

namespace vanta::modules::chams::crash {

// Idempotent. Safe to call before anything else is installed.
void install();
void shutdown();

// Breadcrumb. `where` MUST be a string literal or otherwise immortal — the
// handler may read it after the caller's frame is gone. Cost is one relaxed
// atomic store, so it is affordable on a per-frame path but NOT per draw call.
void mark(const char* where);

} // namespace vanta::modules::chams::crash

// Per-frame breadcrumb. Compiles to a single relaxed store.
#define CHAMS_MARK(where) ::vanta::modules::chams::crash::mark(where)
