#include "inject/Injector.h"
#include "inject/map/ManualMap.h"

namespace wrath::inject {

bool map_stomp(HANDLE hProc, DWORD pid, std::span<const uint8_t> payload,
               const Options& /*opts*/, Result& result) {
    std::wstring status;
    bool ok = wrath::injector::ManualMapInMemoryWithStomp(
        hProc, pid, payload.data(), payload.size(), status);
    result.detail = status;
    // Stomp may fail by size; report the detail, never throw.
    return ok;
}

} // namespace wrath::inject
