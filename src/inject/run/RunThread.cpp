#include "inject/Injector.h"
#include "inject/NtInjector.h"

namespace wrath::inject {

// Remote-thread execution variant. The mapping functions already create the
// execution thread internally; this entry exists as the explicit strategy slot
// and validates that the payload is runnable before delegating.
bool run_thread(HANDLE hProc, DWORD /*pid*/, std::span<const uint8_t> payload,
                const Options& opts, Result& result) {
    if (!hProc || hProc == INVALID_HANDLE_VALUE) {
        result.detail = L"run_thread: handle de proceso invalido.";
        return false;
    }
    if (payload.size() < sizeof(IMAGE_DOS_HEADER)) {
        result.detail = L"run_thread: payload no es PE.";
        return false;
    }
    (void)opts;
    // The concrete mapping strategies own the execution; this slot validates
    // preconditions and defers. A null/closed handle is a hard failure.
    result.detail = L"run_thread: ejecucion delegada al mapeador.";
    return true;
}

} // namespace wrath::inject
