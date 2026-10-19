#include "chams_crash.h"
#include "chams_debug.h"
#include "chams_gl.h"

#include <windows.h>

#include <atomic>

namespace vanta::modules::chams::crash {

namespace {

PVOID             g_handle = nullptr;
std::atomic<const char*> g_mark{ "<nothing yet>" };
std::atomic<int>  g_reported{ 0 };
std::atomic<bool> g_installed{ false };

// Which module owns an address. Written into a caller-supplied buffer because
// the handler must not allocate: a stack-overflow report has almost no stack
// left, and a heap lock may be held by the very thread that faulted.
void owner_of(const void* addr, char* out, DWORD cap)
{
    out[0] = '?';
    out[1] = '\0';
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &m) && m) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(m, full, MAX_PATH)) {
            const char* base = full;
            for (const char* p = full; *p; ++p)
                if (*p == '\\' || *p == '/') base = p + 1;
            lstrcpynA(out, base, static_cast<int>(cap));
        }
    }
}

// Faults the JVM raises as part of normal operation, or that are not fatal.
bool is_fatal(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
            return true;
        default:
            return false;   // 0xE06D7363 (C++ throw), breakpoints, thread naming...
    }
}

const char* name_of(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        default:                              return "OTHER";
    }
}

LONG CALLBACK handler(EXCEPTION_POINTERS* ep)
{
    // Get out of the way FIRST, unconditionally, on every path below.
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD& er = *ep->ExceptionRecord;
    if (!is_fatal(er.ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;

    // THE FILTER. The JVM faults constantly and legitimately; without this the
    // log would be useless noise and the VM measurably slower. A fault is only
    // interesting when a chams detour is on the stack, or when the faulting
    // instruction is inside this DLL.
    const bool in_detour = ::vanta::modules::chams::gl::in_detour_this_thread() > 0;
    bool ours = in_detour;
    if (!ours) {
        static const char anchor = 0;    // an address known to be in this DLL
        HMODULE self = nullptr, faulting = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, &anchor, &self);
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(er.ExceptionAddress), &faulting);
        ours = (self && self == faulting);
    }
    if (!ours) return EXCEPTION_CONTINUE_SEARCH;

    // Cap the volume: a fault inside a per-draw detour can repeat thousands of
    // times a second, and a log that scrolls its own answer away is no better
    // than one that stops.
    if (g_reported.fetch_add(1, std::memory_order_relaxed) >= 8)
        return EXCEPTION_CONTINUE_SEARCH;

    char mod[64];
    owner_of(er.ExceptionAddress, mod, sizeof mod);

    const char* mk = g_mark.load(std::memory_order_relaxed);

    ::vanta::desklog::section(dbg::ch(), "CRASH");
    CHAMS_LOG("crash: %s (0x%08lX) at %p in %s",
              name_of(er.ExceptionCode), (unsigned long)er.ExceptionCode,
              er.ExceptionAddress, mod);
    CHAMS_LOG("crash: last chams breadcrumb = %s", mk ? mk : "<null>");
    CHAMS_LOG("crash: inside a chams GL detour on THIS thread = %s (depth=%d); "
              "process-wide depth=%d",
              in_detour ? "YES" : "no",
              ::vanta::modules::chams::gl::in_detour_this_thread(),
              ::vanta::modules::chams::gl::in_detour());
    CHAMS_LOG("crash: thread=%lu", GetCurrentThreadId());

    if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
        const ULONG_PTR op = er.ExceptionInformation[0];
        CHAMS_LOG("crash: tried to %s address %p",
                  op == 0 ? "READ" : op == 1 ? "WRITE" : "EXECUTE",
                  reinterpret_cast<void*>(er.ExceptionInformation[1]));
    }
    if (er.ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        CHAMS_LOG("crash: STACK OVERFLOW — this is the signature of unbounded "
                  "recursion. The usual cause here is a detour re-entering the "
                  "function it hooked, i.e. a trampoline that points back at the "
                  "detour rather than at the original.");
    }

    // Caller-return addresses off the stack. Not a real unwind — no symbol or
    // frame-pointer walking in a fault handler — but enough to name the modules
    // involved, which is what tells us whose code we died in.
#ifdef _M_X64
    const ULONG_PTR sp = ep->ContextRecord ? ep->ContextRecord->Rsp : 0;
    if (sp) {
        int shown = 0;
        for (int i = 0; i < 256 && shown < 8; ++i) {
            const void** slot = reinterpret_cast<const void**>(sp) + i;
            if (IsBadReadPtr(slot, sizeof(void*))) break;
            const void* val = *slot;
            if (!val) continue;
            char m2[64];
            owner_of(val, m2, sizeof m2);
            if (m2[0] == '?') continue;          // not a code address
            CHAMS_LOG("crash:   stack[%3d] %p  %s", i, val, m2);
            ++shown;
        }
    }
#endif
    ::vanta::desklog::flush(dbg::ch());

    return EXCEPTION_CONTINUE_SEARCH;   // let the JVM produce its own report too
}

} // namespace

void install()
{
    if (g_installed.exchange(true)) return;
    // First in the chain: we want to see the fault before any handler that might
    // swallow it.
    g_handle = AddVectoredExceptionHandler(1, handler);
    CHAMS_LOG("crash: reporter %s", g_handle ? "armed" : "FAILED to arm");
}

void shutdown()
{
    if (!g_installed.exchange(false)) return;
    if (g_handle) { RemoveVectoredExceptionHandler(g_handle); g_handle = nullptr; }
}

void mark(const char* where)
{
    g_mark.store(where, std::memory_order_relaxed);
}

} // namespace vanta::modules::chams::crash
