#include "trace/snapshot.h"
#include "core/nt.h"
#include "core/hash.h"

#include <tlhelp32.h>
#include <vector>

namespace wrath::trace {

namespace {

DWORD find_winlogon_pid() {
    DWORD pid = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"winlogon.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return pid;
}

class SystemImpersonationGuard {
public:
    SystemImpersonationGuard() {
        DWORD pid = find_winlogon_pid();
        if (!pid) return;

        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hProc) return;

        HANDLE hToken = nullptr;
        if (OpenProcessToken(hProc, TOKEN_DUPLICATE, &hToken)) {
            HANDLE hDup = nullptr;
            if (DuplicateTokenEx(
                hToken,
                TOKEN_ALL_ACCESS,
                nullptr,
                SecurityImpersonation,
                TokenImpersonation,
                &hDup
            )) {
                if (SetThreadToken(nullptr, hDup)) {
                    elevated_ = true;
                }
                CloseHandle(hDup);
            }
            CloseHandle(hToken);
        }
        CloseHandle(hProc);
    }

    ~SystemImpersonationGuard() {
        if (elevated_) {
            SetThreadToken(nullptr, nullptr);
        }
    }

    bool is_elevated() const noexcept { return elevated_; }

private:
    bool elevated_ = false;
};

void delete_value_anti_slack(HKEY hKey, const wchar_t* value_name) {
    // Mitigacion de BAM Slack Space (BamDeletedKeys):
    // Sobrescribir la celda asignandole el nombre y valor de un binario nativo
    // (\Device\HarddiskVolume3\Windows\System32\svchost.exe) y forzar consolidacion con RegFlushKey().
    const wchar_t kDecoyBinary[] = L"\\Device\\HarddiskVolume3\\Windows\\System32\\svchost.exe";
    uint8_t decoy_val[8] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };

    // 1. Sobrescribir los datos de la celda existente antes de desvincularla
    DWORD name_bytes = static_cast<DWORD>(wcslen(value_name) * sizeof(wchar_t));
    std::vector<BYTE> scrub_data(name_bytes + sizeof(kDecoyBinary), 0);
    memcpy(scrub_data.data(), kDecoyBinary, sizeof(kDecoyBinary));
    RegSetValueExW(
        hKey,
        value_name,
        0,
        REG_BINARY,
        scrub_data.data(),
        static_cast<DWORD>(scrub_data.size())
    );
    RegFlushKey(hKey);

    // 2. Asignar el valor del binario nativo en la colmena
    RegSetValueExW(
        hKey,
        kDecoyBinary,
        0,
        REG_BINARY,
        decoy_val,
        sizeof(decoy_val)
    );
    RegFlushKey(hKey);

    // 3. Eliminar el valor original garantizando que el espacio residual no contenga cadenas del loader
    RegDeleteValueW(hKey, value_name);
    RegFlushKey(hKey);
}
} // namespace

bool restore_bam(const snapshot_t& snap) {
    std::wstring sid;
    if (!get_current_user_sid(sid)) return false;

    SystemImpersonationGuard guard;
    // Si no somos SYSTEM ni pudimos elevar, RegOpenKeyEx fallará en la clave de BAM

    std::wstring bam_key = L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings\\" + sid;
    HKEY hKey = nullptr;
    LSTATUS status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        bam_key.c_str(),
        0,
        KEY_QUERY_VALUE | KEY_SET_VALUE,
        &hKey
    );
    if (status != ERROR_SUCCESS) return false;

    // Buscar el nombre del valor coincidente
    // La clave puede guardar la ruta completa (ej. K:\Tools\BackupManager.exe)
    // o prefijada con \Device\HarddiskVolumeX\...
    const wchar_t* filename = wcsrchr(snap.loader_path, L'\\');
    filename = filename ? filename + 1 : snap.loader_path;

    std::vector<std::wstring> targets_to_scrub;

    DWORD val_count = 0;
    DWORD max_val_len = 0;
    if (RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         &val_count, &max_val_len, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        std::vector<wchar_t> val_name(max_val_len + 2);
        for (DWORD i = 0; i < val_count; ++i) {
            DWORD len = max_val_len + 1;
            if (RegEnumValueW(hKey, i, val_name.data(), &len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                if (wcsstr(val_name.data(), filename) != nullptr) {
                    targets_to_scrub.push_back(val_name.data());
                }
            }
        }
    }

    if (snap.bam_exist) {
        // Restaurar el valor original si existía
        RegSetValueExW(
            hKey,
            snap.loader_path,
            0,
            REG_BINARY,
            snap.bam,
            sizeof(snap.bam)
        );
        RegFlushKey(hKey);
    } else {
        // Eliminar con anti-slack
        for (const auto& target : targets_to_scrub) {
            delete_value_anti_slack(hKey, target.c_str());
        }
    }

    RegCloseKey(hKey);
    return true;
}

} // namespace wrath::trace
