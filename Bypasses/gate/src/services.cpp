#include "services.h"
#include <map>
#include <vector>

namespace wrath::gate {

namespace {

const std::vector<std::wstring> kMandatoryServices = {
    L"DcomLaunch",
    L"RpcSs",
    L"EventLog",
    L"Schedule",
    L"AppInfo",
    L"DPS",
    L"DiagTrack",
    L"DUSMSvc",
    L"WSearch",
    L"SysMain",
    L"PcaSvc"
};

std::wstring state_to_string(DWORD state) {
    switch (state) {
        case SERVICE_STOPPED: return L"STOPPED";
        case SERVICE_START_PENDING: return L"START_PENDING";
        case SERVICE_STOP_PENDING: return L"STOP_PENDING";
        case SERVICE_RUNNING: return L"RUNNING";
        case SERVICE_CONTINUE_PENDING: return L"CONTINUE_PENDING";
        case SERVICE_PAUSE_PENDING: return L"PAUSE_PENDING";
        case SERVICE_PAUSED: return L"PAUSED";
        default: return L"UNKNOWN(" + std::to_wstring(state) + L")";
    }
}

} // namespace

ServicesAuditResult audit_services() {
    ServicesAuditResult result{};
    result.fail_mask = 0;
    result.all_passed = true;

    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (!hSCM) {
        ServiceCheckResult err{};
        err.name = L"SCM";
        err.expected = L"CONNECT";
        err.actual = L"ACCESS_DENIED";
        err.passed = false;
        err.detail = L"Error abriendo Service Control Manager: " + std::to_wstring(GetLastError());
        result.results.push_back(err);
        result.all_passed = false;
        result.fail_mask = 0xFFF;
        return result;
    }

    // Mapa de servicios detectados
    std::map<std::wstring, DWORD> found_states;

    DWORD bytes_needed = 0;
    DWORD services_returned = 0;
    DWORD resume_handle = 0;

    DWORD buf_size = 256 * 1024;
    std::vector<BYTE> buffer(buf_size);

    BOOL ok = EnumServicesStatusExW(
        hSCM,
        SC_ENUM_PROCESS_INFO,
        SERVICE_WIN32 | SERVICE_DRIVER,
        SERVICE_STATE_ALL,
        buffer.data(),
        buf_size,
        &bytes_needed,
        &services_returned,
        &resume_handle,
        nullptr
    );

    if (!ok && GetLastError() == ERROR_MORE_DATA) {
        buf_size += bytes_needed + 32 * 1024;
        buffer.resize(buf_size);
        resume_handle = 0;
        ok = EnumServicesStatusExW(
            hSCM,
            SC_ENUM_PROCESS_INFO,
            SERVICE_WIN32 | SERVICE_DRIVER,
            SERVICE_STATE_ALL,
            buffer.data(),
            buf_size,
            &bytes_needed,
            &services_returned,
            &resume_handle,
            nullptr
        );
    }

    if (ok) {
        auto* services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
        for (DWORD i = 0; i < services_returned; ++i) {
            if (services[i].lpServiceName) {
                found_states[services[i].lpServiceName] = services[i].ServiceStatusProcess.dwCurrentState;
            }
        }
    }

    // 1. Auditar los 11 servicios Win32
    for (size_t i = 0; i < kMandatoryServices.size(); ++i) {
        const auto& name = kMandatoryServices[i];
        ServiceCheckResult r{};
        r.name = name;
        r.expected = L"RUNNING";

        auto it = found_states.find(name);
        if (it != found_states.end()) {
            r.actual = state_to_string(it->second);
            r.passed = (it->second == SERVICE_RUNNING);
        } else {
            // Intento directo con OpenService si EnumServices no lo listó
            SC_HANDLE hSvc = OpenServiceW(hSCM, name.c_str(), SERVICE_QUERY_STATUS);
            if (hSvc) {
                SERVICE_STATUS_PROCESS ssp{};
                DWORD dummy = 0;
                if (QueryServiceStatusEx(hSvc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &dummy)) {
                    r.actual = state_to_string(ssp.dwCurrentState);
                    r.passed = (ssp.dwCurrentState == SERVICE_RUNNING);
                } else {
                    r.actual = L"QUERY_FAILED";
                    r.passed = false;
                }
                CloseServiceHandle(hSvc);
            } else {
                r.actual = L"NOT_FOUND";
                r.passed = false;
            }
        }

        if (!r.passed) {
            result.fail_mask |= (1u << i);
            result.all_passed = false;
        }
        result.results.push_back(r);
    }

    // 2. Auditar BAM (driver kernel)
    {
        ServiceCheckResult r{};
        r.name = L"bam";
        r.expected = L"RUNNING";

        SC_HANDLE hBam = OpenServiceW(hSCM, L"bam", SERVICE_QUERY_STATUS);
        if (hBam) {
            SERVICE_STATUS_PROCESS ssp{};
            DWORD dummy = 0;
            if (QueryServiceStatusEx(hBam, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &dummy)) {
                r.actual = state_to_string(ssp.dwCurrentState);
                r.passed = (ssp.dwCurrentState == SERVICE_RUNNING);
            } else {
                r.actual = L"QUERY_FAILED";
                r.passed = false;
            }
            CloseServiceHandle(hBam);
        } else {
            r.actual = L"NOT_FOUND/NO_ACCESS";
            r.passed = false;
            r.detail = L"Error: " + std::to_wstring(GetLastError());
        }

        if (!r.passed) {
            result.fail_mask |= (1u << 11);
            result.all_passed = false;
        }
        result.results.push_back(r);
    }

    CloseServiceHandle(hSCM);
    return result;
}

std::vector<ServiceCheckResult> audit_system_config() {
    std::vector<ServiceCheckResult> checks;

    // EnablePrefetcher & EnableSuperfetch en Memory Management
    {
        ServiceCheckResult r{};
        r.name = L"EnablePrefetcher";
        r.expected = L"3";
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters",
            0,
            KEY_QUERY_VALUE,
            &hKey
        ) == ERROR_SUCCESS) {
            DWORD val = 0;
            DWORD sz = sizeof(val);
            if (RegQueryValueExW(hKey, L"EnablePrefetcher", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS) {
                r.actual = std::to_wstring(val);
                r.passed = (val == 3);
            } else {
                r.actual = L"NOT_SET";
                r.passed = false;
            }
            RegCloseKey(hKey);
        } else {
            r.actual = L"KEY_NOT_FOUND";
            r.passed = false;
        }
        checks.push_back(r);
    }

    // Windows Search: SetupCompletedSuccessfully = 1
    {
        ServiceCheckResult r{};
        r.name = L"WindowsSearchSetup";
        r.expected = L"1";
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows Search",
            0,
            KEY_QUERY_VALUE,
            &hKey
        ) == ERROR_SUCCESS) {
            DWORD val = 0;
            DWORD sz = sizeof(val);
            if (RegQueryValueExW(hKey, L"SetupCompletedSuccessfully", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS) {
                r.actual = std::to_wstring(val);
                r.passed = (val == 1);
            } else {
                r.actual = L"NOT_SET";
                r.passed = false;
            }
            RegCloseKey(hKey);
        } else {
            r.actual = L"KEY_NOT_FOUND";
            r.passed = false;
        }
        checks.push_back(r);
    }

    return checks;
}

} // namespace wrath::gate
