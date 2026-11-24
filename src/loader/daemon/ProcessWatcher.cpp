#include "loader/daemon/ProcessWatcher.h"
#include "loader/launcher_injector.h"
#include "loader/Paths.h"
#include "trace/snapshot.h"
#include "trace/teardown.h"

#include <windows.h>
#include <tlhelp32.h>
#include <chrono>
#include <thread>
#include <iostream>

namespace wrath::daemon {

namespace {

constexpr const wchar_t* kStartupKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kStartupValueName = L"DisplayTunerService";

} // namespace

ProcessWatcher::ProcessWatcher() = default;

ProcessWatcher::~ProcessWatcher() {
    Stop();
}

void ProcessWatcher::Stop() {
    m_should_stop.store(true);
}

bool ProcessWatcher::IsProcessReadyForInjection(DWORD pid) {
    if (pid == 0) return false;

    // Si ya respondio el centinela, ya esta listo e inyectado
    if (launcher::IsWrathLoadedInProcess(pid)) return true;

    // Verificar si jvm.dll esta cargada en el proceso remoto
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) {
        // Si no se puede enumerar modulos (e.g. proceso recien naciendo), esperar
        return false;
    }

    bool has_jvm = false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    if (Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"jvm.dll") == 0) {
                has_jvm = true;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);

    return has_jvm;
}

int ProcessWatcher::RunBlocking(const WatcherConfig& config, std::vector<uint8_t> payload_bytes) {
    m_running.store(true);
    m_should_stop.store(false);

    WatchLoop(config, payload_bytes);

    m_running.store(false);
    return 0;
}

void ProcessWatcher::WatchLoop(const WatcherConfig& config, const std::vector<uint8_t>& payload_bytes) {
    if (payload_bytes.empty()) {
        if (config.verbose) {
            std::wcerr << L"[-] Daemon: Error, buffer de payload vacio en memoria.\n";
        }
        return;
    }

    wchar_t self_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self_path, MAX_PATH);

    if (config.verbose) {
        std::wcout << L"[+] DisplayTunerService Daemon iniciado.\n";
        std::wcout << L"[*] Monitoreando creacion de javaw.exe en segundo plano...\n";
    }

    while (!m_should_stop.load()) {
        const auto procs = launcher::ScanMinecraftProcesses();
        if (!procs.empty()) {
            for (const auto& proc : procs) {
                if (m_should_stop.load()) break;

                const DWORD pid = proc.pid;
                if (pid == 0) continue;

                // Si ya esta inyectado, esperar a que termine para no duplicar
                if (launcher::IsWrathLoadedInProcess(pid)) {
                    // Monitorizar hasta que cierre
                    HANDLE hSync = OpenProcess(SYNCHRONIZE, FALSE, pid);
                    if (hSync) {
                        while (!m_should_stop.load()) {
                            if (WaitForSingleObject(hSync, 1000) == WAIT_OBJECT_0) {
                                break;
                            }
                        }
                        CloseHandle(hSync);
                    }
                    continue;
                }

                // Esperar a que jvm.dll este completamente mapeada
                if (!IsProcessReadyForInjection(pid)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    continue;
                }

                // Margen de seguridad para estabilizacion de ventana GLFW
                std::this_thread::sleep_for(std::chrono::milliseconds(500));

                if (config.verbose) {
                    std::wcout << L"[+] Detectado target: " << proc.exe_name << L" (PID: " << pid << L")\n";
                    std::wcout << L"[*] Capturando snapshot forense previo a la inyeccion...\n";
                }

                wrath::trace::snapshot_t snap{};
                wrath::trace::snapshot_artifacts(self_path, &snap);

                std::wstring status;
                bool ok = launcher::InjectPayloadFromMemoryIntoProcess(
                    pid,
                    payload_bytes,
                    status,
                    config.server_host,
                    config.server_port,
                    config.hwid,
                    "", "", "",
                    config.injection_options
                );

                if (config.verbose) {
                    if (ok) {
                        std::wcout << L"[+] Inyeccion desatendida completada con exito.\n";
                    } else {
                        std::wcout << L"[-] Fallo en inyeccion: " << status << L"\n";
                    }
                }

                if (ok) {
                    // Esperar en reposo a que el proceso de Minecraft termine
                    HANDLE hWaitProc = OpenProcess(SYNCHRONIZE, FALSE, pid);
                    if (hWaitProc) {
                        while (!m_should_stop.load()) {
                            if (WaitForSingleObject(hWaitProc, 1000) == WAIT_OBJECT_0) {
                                break;
                            }
                        }
                        CloseHandle(hWaitProc);
                    }

                    if (config.auto_teardown_on_exit) {
                        if (config.verbose) {
                            std::wcout << L"[*] Minecraft cerrado. Ejecutando teardown forense automatico...\n";
                        }
                        wrath::trace::teardown(snap);
                    }
                }

                wrath::trace::free_snapshot(&snap);
            }
        }

        // Reposo en busqueda pasiva para consumo 0% CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(config.poll_interval_ms));
    }

    if (config.verbose) {
        std::wcout << L"[*] Daemon detenido limpiamente.\n";
    }
}

bool ProcessWatcher::InstallStartupEntry(const std::wstring& exe_path) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupKeyPath, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    // Registrar comando: "<path>" --daemon
    std::wstring cmd = L"\"" + exe_path + L"\" --daemon";
    LONG res = RegSetValueExW(
        hKey,
        kStartupValueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(cmd.c_str()),
        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t))
    );

    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS);
}

bool ProcessWatcher::UninstallStartupEntry() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupKeyPath, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    LONG res = RegDeleteValueW(hKey, kStartupValueName);
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS || res == ERROR_FILE_NOT_FOUND);
}

bool ProcessWatcher::IsStartupEntryInstalled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupKeyPath, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    LONG res = RegQueryValueExW(hKey, kStartupValueName, nullptr, &type, nullptr, nullptr);
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS);
}

} // namespace wrath::daemon
