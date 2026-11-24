#pragma once
#ifndef WRATH_AUTH_CLIENT_H
#define WRATH_AUTH_CLIENT_H

#include <string>
#include <vector>
#include <cstdint>
#include <windows.h>

namespace wrath::auth
{

enum class SessionState
{
    Pending,
    Approved,
    Rejected,
    Expired,
    Error
};

// Obtiene el identificador de máquina (HWID) único del PC actual
std::string GetMachineHWID();

// Intento de auto-login usando el HWID si la máquina ya fue aprobada previamente
bool RequestAutoLogin(
    const std::string& server_host,
    int port,
    const std::string& hwid,
    std::string& out_download_token,
    std::string& out_message);

// Paso 1: Enviar código generado por Discord (/download) e iniciar la alerta de 2FA
bool RequestDiscordAuth(
    const std::string& server_host,
    int port,
    const std::string& discord_code,
    const std::string& hwid,
    std::string& out_session_id,
    std::string& out_message);

// Paso 2: Consultar periódicamente si el usuario ya presionó "Permitir" en Discord
SessionState PollDiscordSession(
    const std::string& server_host,
    int port,
    const std::string& session_id,
    std::string& out_download_token);

// Paso 3: Descargar y descifrar el DLL con intercambio efímero ECDH P-256 + HKDF + AES-256-GCM directamente en RAM
bool FetchAndDecryptPayload(
    const std::string& server_host,
    int port,
    const std::string& download_token,
    const std::string& hwid,
    std::vector<uint8_t>& out_dll_bytes,
    std::string& out_error,
    std::string* out_heartbeat_token = nullptr,
    std::string* out_session_id = nullptr,
    std::string* out_client_nonce = nullptr);


} // namespace wrath::auth

#endif // WRATH_AUTH_CLIENT_H
