#include "diagnostics/SessionDiagnostics.h"
#include <nlohmann/json.hpp>
#include <iostream>

int main(int argc, char**) {
    void* allocation = VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!allocation) return 1;
    DWORD previous = 0;
    if (!VirtualProtect(static_cast<char*>(allocation) + 4096, 4096, PAGE_READONLY, &previous)) return 2;
    const auto directory = std::filesystem::current_path()
        / (L"diagnostics-test-" + std::to_wstring(GetCurrentProcessId()));
    wrath::diagnostics::recordModule(reinterpret_cast<HMODULE>(allocation), directory);
    nlohmann::json snapshot;
    std::ifstream file(directory / (L"module-" + std::to_wstring(GetCurrentProcessId()) + L".json"));
    if (!file || !(file >> snapshot)) return 4;
    const auto base = reinterpret_cast<std::uintptr_t>(allocation);
    if (snapshot.at("allocation_base").get<std::uintptr_t>() != base
        || snapshot.at("allocation_size").get<size_t>() != 8192
        || snapshot.at("registered_at_start").get<bool>()
        || snapshot.at("pid").get<DWORD>() != GetCurrentProcessId()
        || snapshot.at("process_start").get<unsigned long long>() == 0) return 5;
    wrath::diagnostics::recordUnloadRequest(directory);
    if (!VirtualFree(allocation, 0, MEM_RELEASE)) return 6;
    if (!wrath::diagnostics::writeRecord(L"test-ready", GetCurrentProcessId(), GetCurrentProcess(),
        ",\"fixture\":true", directory)) return 7;
    std::cout << "Allocation metadata, process identity and diagnostic JSON verified\n";
    // Optional lifetime for the external read-only observer integration check.
    if (argc > 1) Sleep(45000);
    return 0;
}
