#pragma once
#ifndef DT_CORE_HEARTBEAT_WATCHDOG_H
#define DT_CORE_HEARTBEAT_WATCHDOG_H

#include <string>
#include <atomic>
#include <cstdint>

namespace dt::sync {

class HeartbeatWatchdog {
public:
    static HeartbeatWatchdog& get();

    // Inicia el hilo supervisor de latidos criptográficos
    bool start();

    // Detiene el hilo supervisor
    void stop();

    bool isRunning() const noexcept { return m_running.load(std::memory_order_relaxed); }
    bool isAuthorized() const noexcept { return m_authorized.load(std::memory_order_relaxed); }

    void disableClientSafely(const char* reason = nullptr);

private:
    HeartbeatWatchdog() = default;
    ~HeartbeatWatchdog();

    void workerLoop();

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_authorized{false};
};

} // namespace dt::sync

#endif // DT_CORE_HEARTBEAT_WATCHDOG_H

