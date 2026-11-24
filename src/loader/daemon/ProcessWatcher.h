#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include <atomic>
#include <cstdint>
#include "inject/Injector.h"

namespace wrath::daemon {

struct WatcherConfig {
    wrath::inject::Options injection_options;
    std::string server_host;
    int server_port = 0;
    std::string hwid;
    bool auto_teardown_on_exit = true;
    uint32_t poll_interval_ms = 750;
    bool verbose = false;
};

class ProcessWatcher {
public:
    ProcessWatcher();
    ~ProcessWatcher();

    // Ejecuta el daemon de vigilancia bloqueando el hilo actual
    int RunBlocking(const WatcherConfig& config, std::vector<uint8_t> payload_bytes);

    // Detiene el bucle de vigilancia
    void Stop();

    [[nodiscard]] bool IsRunning() const noexcept { return m_running.load(); }

    // Helpers de persistencia en arranque (HKCU\Run sin privilegios ruidosos)
    static bool InstallStartupEntry(const std::wstring& exe_path);
    static bool UninstallStartupEntry();
    static bool IsStartupEntryInstalled();

private:
    void WatchLoop(const WatcherConfig& config, const std::vector<uint8_t>& payload_bytes);
    bool IsProcessReadyForInjection(DWORD pid);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_should_stop{false};
};

} // namespace wrath::daemon
