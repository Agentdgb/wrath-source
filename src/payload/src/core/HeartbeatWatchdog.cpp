#include "HeartbeatWatchdog.h"

#include "core/module/ModuleManager.h"
#include "modules/combat/autototem/autototem.h"
#include "modules/combat/triggerbot/triggerbot.h"
#include "modules/combat/silentaim/silentaim.h"
#include "platform/ThreadUtil.h"
#include "util/Log.h"
#include "shared/stealth/StealthDefs.h"
#include "vgui/vgui.h"

#include <windows.h>
#include <winhttp.h>
#include <intrin.h>
#include <sstream>
#include <vector>
#include <chrono>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace vanta::sync {

namespace {

struct ScopedWinHttp {
    HINTERNET h = nullptr;
    ~ScopedWinHttp() { if (h) WinHttpCloseHandle(h); }
    HINTERNET get() const noexcept { return h; }
    operator HINTERNET() const noexcept { return h; }
    HINTERNET* operator&() noexcept { return &h; }
};

std::wstring ToWide(const std::string& str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring w(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), w.data(), size);
    return w;
}

std::string ExtractJsonField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t key_pos = json.find(needle);
    if (key_pos == std::string::npos) return {};

    size_t colon_pos = json.find(':', key_pos + needle.length());
    if (colon_pos == std::string::npos) return {};

    size_t val_start = colon_pos + 1;
    while (val_start < json.size() && (json[val_start] == ' ' || json[val_start] == '\t' || json[val_start] == '\r' || json[val_start] == '\n'))
        val_start++;

    if (val_start >= json.size()) return {};

    if (json[val_start] == '\"') {
        val_start++;
        size_t val_end = json.find('\"', val_start);
        if (val_end != std::string::npos)
            return json.substr(val_start, val_end - val_start);
    } else {
        size_t val_end = json.find_first_of(",}\r\n \t", val_start);
        if (val_end != std::string::npos)
            return json.substr(val_start, val_end - val_start);
        return json.substr(val_start);
    }
    return {};
}

std::string ComputeFallbackHWID() {
    char guid[256]{};
    DWORD size = sizeof(guid);
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "MachineGuid", nullptr, nullptr, reinterpret_cast<LPBYTE>(guid), &size);
        RegCloseKey(hKey);
    }

    DWORD vol_serial = 0;
    GetVolumeInformationA("C:\\", nullptr, 0, &vol_serial, nullptr, nullptr, nullptr, 0);

    int cpu_info[4]{};
    __cpuid(cpu_info, 0);

    char raw[256]{};
    std::snprintf(raw, sizeof(raw), "%s;%08X;%08X-%08X", guid, vol_serial, cpu_info[0], cpu_info[1]);
    return std::string(raw);
}

} // namespace

HeartbeatWatchdog& HeartbeatWatchdog::get() {
    static HeartbeatWatchdog instance;
    return instance;
}

HeartbeatWatchdog::~HeartbeatWatchdog() {
    stop();
}

bool HeartbeatWatchdog::start() {
    if (m_running.exchange(true))
        return true;

    m_stopRequested.store(false);
    m_authorized.store(true);

    if (!vanta::platform::spawnNamed("Heartbeat Watchdog", [this] {
        workerLoop();
    })) {
        m_running.store(false);
        VLOG_ERR("auth", "Fallo al iniciar el hilo de supervisión de latidos.");
        return false;
    }

    VLOG_INFO("auth", "Supervisor de sesión y latido remoto activado.");
    return true;
}

void HeartbeatWatchdog::stop() {
    m_stopRequested.store(true);
    for (int i = 0; i < 50 && m_running.load(); ++i) {
        Sleep(20);
    }
}

void HeartbeatWatchdog::disableClientSafely(const char* reason) {
    m_authorized.store(false);
    VLOG_WARN("auth", "Desactivación segura de funciones: {}", reason ? reason : "sesión no autorizada");

    // 1. Apagar limpiamente todos los módulos sin descargar DLL ni desestabilizar OpenGL
    try {
        vanta::ModuleManager::get().panic();
    } catch (...) {}

    // 2. Levantar flags de shutdown en combate para neutralizar cualquier hook de JVM en vuelo
    vanta::modules::autototem::g_shutting_down.store(true, std::memory_order_release);
    vanta::modules::triggerbot::g_shutting_down.store(true, std::memory_order_release);
    vanta::modules::silentaim::g_shutting_down.store(true, std::memory_order_release);

    // 3. Ocultar el menú de interfaz si estaba visible
    try {
        if (vanta::vgui::isVisible()) {
            vanta::vgui::toggleVisible();
        }
    } catch (...) {}
}

void HeartbeatWatchdog::workerLoop() {
    std::string host = "127.0.0.1";
    int port = 8000;
    std::string hwid;
    std::string session_token;
    std::string session_id;
    std::string client_nonce;
    uint32_t sequence = 0;

    // 1. Reintentar leer contexto de memoria compartida durante los primeros segundos
    const DWORD pid = GetCurrentProcessId();
    std::wstring secName = wrath::bypass::heartbeatSectionNameForPid(pid);

    for (int retry = 0; retry < 10 && !m_stopRequested.load(); ++retry) {
        HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, secName.c_str());
        if (hMap) {
            auto* ctx = reinterpret_cast<const wrath::bypass::HeartbeatSharedContext*>(
                MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(wrath::bypass::HeartbeatSharedContext)));
            if (ctx && ctx->magic == wrath::bypass::kHeartbeatMagic) {
                if (ctx->server_host[0]) host = ctx->server_host;
                if (ctx->server_port > 0) port = ctx->server_port;
                if (ctx->hwid[0]) hwid = ctx->hwid;
                if (ctx->session_token[0]) session_token = ctx->session_token;
                if (ctx->session_id[0]) session_id = ctx->session_id;
                if (ctx->client_nonce[0]) client_nonce = ctx->client_nonce;
                VLOG_INFO("auth", "Contexto de sesión cargado exitosamente para PID {} ({}:{})", pid, host, port);
            }
            if (ctx) UnmapViewOfFile(ctx);
            CloseHandle(hMap);
            if (!session_token.empty())
                break;
        }
        Sleep(500);
    }

    if (hwid.empty()) {
        hwid = ComputeFallbackHWID();
    }

    // Si no hay token de sesión (ej. inyección local o sin loader), mantenemos el cliente
    // operativo en modo local/desarrollo sin tumbar Minecraft.
    if (session_token.empty()) {
        VLOG_INFO("auth", "Sesión iniciada sin token remoto; operando en modo local seguro.");
    }

    int consecutiveFailures = 0;

    // Ciclo principal de supervisión cada 30 segundos
    while (!m_stopRequested.load()) {
        for (int i = 0; i < 120 && !m_stopRequested.load(); ++i) {
            Sleep(250);
        }

        if (m_stopRequested.load())
            break;

        if (session_token.empty()) {
            continue;
        }

        // Construir JSON del latido
        std::string json_body = "{\"session_id\":\"" + session_id + "\","
                                "\"session_token\":\"" + session_token + "\","
                                "\"hwid\":\"" + hwid + "\","
                                "\"client_nonce\":\"" + client_nonce + "\","
                                "\"sequence\":" + std::to_string(sequence) + "}";

        ScopedWinHttp session;
        session.h = WinHttpOpen(L"WrathPayload-Heartbeat/3.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session.get()) {
            continue;
        }

        WinHttpSetTimeouts(session.get(), 2000, 2000, 3000, 3000);

        ScopedWinHttp connect;
        connect.h = WinHttpConnect(session.get(), ToWide(host).c_str(), static_cast<INTERNET_PORT>(port), 0);
        if (!connect.get()) {
            consecutiveFailures++;
            if (consecutiveFailures >= 5) {
                disableClientSafely("Servidor no alcanzable tras múltiples reintentos");
            }
            continue;
        }

        const DWORD flags = (port == 443) ? WINHTTP_FLAG_SECURE : 0;
        ScopedWinHttp request;
        request.h = WinHttpOpenRequest(connect.get(), L"POST", L"/api/v1/session/heartbeat", nullptr,
                                       nullptr, nullptr, flags);
        if (!request.get()) {
            continue;
        }

        std::wstring headers = L"Content-Type: application/json\r\n";
        BOOL sent = WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.length()),
                                       (LPVOID)json_body.c_str(), static_cast<DWORD>(json_body.length()),
                                       static_cast<DWORD>(json_body.length()), 0);

        if (!sent || !WinHttpReceiveResponse(request.get(), nullptr)) {
            consecutiveFailures++;
            if (consecutiveFailures >= 5) {
                disableClientSafely("Pérdida prolongada de comunicación con el servidor");
            }
            continue;
        }

        DWORD statusCode = 0;
        DWORD dwSize = sizeof(statusCode);
        WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode == 200) {
            std::string response_body;
            DWORD bytes_avail = 0;
            while (WinHttpQueryDataAvailable(request.get(), &bytes_avail) && bytes_avail > 0) {
                size_t prev_size = response_body.size();
                response_body.resize(prev_size + bytes_avail);
                DWORD bytes_read = 0;
                if (WinHttpReadData(request.get(), response_body.data() + prev_size, bytes_avail, &bytes_read))
                    response_body.resize(prev_size + bytes_read);
            }

            std::string next_token = ExtractJsonField(response_body, "next_token");
            if (!next_token.empty()) {
                session_token = next_token;
            }
            consecutiveFailures = 0;
            sequence++;
            m_authorized.store(true);
            vanta::modules::autototem::g_shutting_down.store(false, std::memory_order_release);
            vanta::modules::triggerbot::g_shutting_down.store(false, std::memory_order_release);
            vanta::modules::silentaim::g_shutting_down.store(false, std::memory_order_release);
            VLOG_INFO("auth", "Latido confirmado con éxito (seq={})", sequence);
        } else if (statusCode == 401 || statusCode == 403) {
            // Revocación remota explícita desde Discord (/resethwid o botón Denegar)
            disableClientSafely("Sesión de juego revocada o HWID restablecido");
            break;
        } else if (statusCode == 503) {
            // Modo mantenimiento activado (/toggleauth)
            disableClientSafely("Servidor en mantenimiento temporal");
            break;
        }
    }

    m_running.store(false);
    VLOG_INFO("auth", "Hilo de supervisión de latidos finalizado.");
}

} // namespace vanta::sync

