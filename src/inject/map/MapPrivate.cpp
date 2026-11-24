#include "inject/Injector.h"
#include "inject/map/ManualMap.h"

namespace wrath::inject {

bool map_private(HANDLE hProc, DWORD /*pid*/, std::span<const uint8_t> payload,
                 const Options& /*opts*/, Result& result) {
    std::wstring status;
    bool ok = wrath::injector::ManualMapInMemory(
        hProc, payload.data(), payload.size(), status);
    result.detail = status;
    return ok;
}

} // namespace wrath::inject
