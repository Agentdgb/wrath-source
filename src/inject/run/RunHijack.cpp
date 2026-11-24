#include "inject/Injector.h"
#include "inject/NtInjector.h"

namespace wrath::inject {

// Thread-hijack execution variant. Like run_thread, the concrete hijack lives
// in ManualMap.h (HijackExec); this is the explicit strategy slot.
bool run_hijack(HANDLE hProc, DWORD /*pid*/, std::span<const uint8_t> payload,
                const Options& opts, Result& result) {
    if (!hProc || hProc == INVALID_HANDLE_VALUE) {
        result.detail = L"run_hijack: handle de proceso invalido.";
        return false;
    }
    if (payload.size() < sizeof(IMAGE_DOS_HEADER)) {
        result.detail = L"run_hijack: payload no es PE.";
        return false;
    }
    (void)opts;
    result.detail = L"run_hijack: ejecucion por thread hijack delegada al mapeador.";
    return true;
}

} // namespace wrath::inject
