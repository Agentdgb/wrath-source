#pragma once
#ifndef WRATH_GATE_SERVICES_H
#define WRATH_GATE_SERVICES_H

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace wrath::gate {

struct ServiceCheckResult {
    std::wstring name;
    std::wstring expected;
    std::wstring actual;
    bool passed;
    std::wstring detail;
};

struct ServicesAuditResult {
    std::vector<ServiceCheckResult> results;
    uint32_t fail_mask; // bits 0-11
    bool all_passed;
};

// Enumera los 12 servicios obligatorios usando EnumServicesStatusExW y OpenServiceW
ServicesAuditResult audit_services();

// Verifica configuración adicional del sistema (EnablePrefetcher, etc.)
std::vector<ServiceCheckResult> audit_system_config();

} // namespace wrath::gate

#endif // WRATH_GATE_SERVICES_H
