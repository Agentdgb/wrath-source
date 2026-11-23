#include "AuthClient.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <intrin.h>
#include <sstream>
#include <array>
#include <memory>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif
#include "ObfuscatedString.h"

namespace wrath::auth
{

namespace
{

struct WinHttpHandleDeleter
{
    void operator()(HINTERNET h) const
    {
        if (h) WinHttpCloseHandle(h);
    }
};
using ScopedHInternet = std::unique_ptr<void, WinHttpHandleDeleter>;

std::wstring ToWide(const std::string& str)
{
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring w(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), w.data(), size);
    return w;
}

std::string ExtractJsonField(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    size_t search_pos = 0;

    while (search_pos < json.size())
    {
        size_t key_pos = json.find(needle, search_pos);
        if (key_pos == std::string::npos) return {};

        // Validar que la clave encontrada esté seguida de dos puntos ':'
        size_t colon_pos = key_pos + needle.length();
        while (colon_pos < json.size() && (json[colon_pos] == ' ' || json[colon_pos] == '\t' || json[colon_pos] == '\r' || json[colon_pos] == '\n'))
            colon_pos++;

        if (colon_pos < json.size() && json[colon_pos] == ':')
        {
            size_t val_pos = colon_pos + 1;
            while (val_pos < json.size() && (json[val_pos] == ' ' || json[val_pos] == '\t' || json[val_pos] == '\r' || json[val_pos] == '\n'))
                val_pos++;

            if (val_pos >= json.size()) return {};

            if (json[val_pos] == '\"')
            {
                // String JSON: parsear preservando y decodificando caracteres de escape (\", \\, \n, etc.)
                val_pos++;
                std::string result;
                result.reserve(64);
                bool escaped = false;

                for (size_t i = val_pos; i < json.size(); ++i)
                {
                    char c = json[i];
                    if (escaped)
                    {
                        switch (c)
                        {
                        case '\"': result.push_back('\"'); break;
                        case '\\': result.push_back('\\'); break;
                        case '/':  result.push_back('/');  break;
                        case 'b':  result.push_back('\b'); break;
                        case 'f':  result.push_back('\f'); break;
                        case 'n':  result.push_back('\n'); break;
                        case 'r':  result.push_back('\r'); break;
                        case 't':  result.push_back('\t'); break;
                        default:   result.push_back(c);    break;
                        }
                        escaped = false;
                    }
                    else if (c == '\\')
                    {
                        escaped = true;
                    }
                    else if (c == '\"')
                    {
                        return result; // Cierre legítimo de comillas
                    }
                    else
                    {
                        result.push_back(c);
                    }
                }
                return result;
            }
            else
            {
                // Valor booleano, numérico o null
                size_t end = json.find_first_of(",}\r\n \t", val_pos);
                if (end != std::string::npos)
                    return json.substr(val_pos, end - val_pos);
                return json.substr(val_pos);
            }
        }

        search_pos = key_pos + needle.length();
    }

    return {};
}

static std::string ComputeSha256Hex(const std::string& input)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return {};

    BCRYPT_HASH_HANDLE hHash = nullptr;
    DWORD cbHash = 32;
    DWORD cbHashObject = 0;
    DWORD cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&cbHashObject), sizeof(DWORD), &cbData, 0);

    std::vector<UCHAR> hashObject(cbHashObject);
    std::vector<UCHAR> hash(cbHash);

    if (NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, hashObject.data(), cbHashObject, nullptr, 0, 0)))
    {
        BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0);
        BCryptFinishHash(hHash, hash.data(), cbHash, 0);
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);

    std::string hex;
    hex.reserve(64);
    char buf[4];
    for (UCHAR b : hash)
    {
        std::snprintf(buf, sizeof(buf), "%02X", b);
        hex.append(buf);
    }
    return hex;
}

static std::string ToHex(const uint8_t* data, size_t len)
{
    std::string hex;
    hex.reserve(len * 2);
    char buf[3];
    for (size_t i = 0; i < len; ++i)
    {
        std::snprintf(buf, sizeof(buf), "%02x", data[i]);
        hex.append(buf);
    }
    return hex;
}

// Pin SHA-256 de la SPKI del certificado leaf del servidor de auth.
// Reemplazar con el valor real del servidor:
//   openssl x509 -in server.crt -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256
static const char* kExpectedServerPinSha256 = "c2ab760203ce7879ea0f464b5117af768fca6720149cc54e651e2fcdcc9b24cf";

static bool ValidateServerPin(HINTERNET request)
{
    if (!kExpectedServerPinSha256 || std::string(kExpectedServerPinSha256) == "REPLACE_WITH_SERVER_SPKI_SHA256_HEX")
        return true;

    PCCERT_CONTEXT pCertContext = nullptr;
    DWORD dwSize = sizeof(pCertContext);
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &pCertContext, &dwSize) || !pCertContext)
        return true; // Conexion HTTP plana o local (sin TLS), omitir validacion de pin SSL

    const auto& spki = pCertContext->pCertInfo->SubjectPublicKeyInfo;
    std::string spkiDer(reinterpret_cast<const char*>(spki.PublicKey.pbData), spki.PublicKey.cbData);
    std::string pin = ComputeSha256Hex(spkiDer);
    CertFreeCertificateContext(pCertContext);

    return pin == kExpectedServerPinSha256;
}

static std::vector<uint8_t> HmacSha256(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return {};

    BCRYPT_HASH_HANDLE hHash = nullptr;
    DWORD cbHash = 32;
    DWORD cbHashObj = 0;
    DWORD cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&cbHashObj), sizeof(DWORD), &cbData, 0);

    std::vector<uint8_t> hashObj(cbHashObj);
    std::vector<uint8_t> out(cbHash);

    if (NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, hashObj.data(), cbHashObj,
                                    const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0)))
    {
        BCryptHashData(hHash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0);
        BCryptFinishHash(hHash, out.data(), cbHash, 0);
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return out;
}

static std::vector<uint8_t> HkdfSha256(const std::vector<uint8_t>& ikm, const std::vector<uint8_t>& salt, const std::vector<uint8_t>& info, size_t len)
{
    // RFC 5869 Extract & Expand
    std::vector<uint8_t> prk = HmacSha256(salt, ikm);
    std::vector<uint8_t> info_plus_counter = info;
    info_plus_counter.push_back(0x01);
    std::vector<uint8_t> okm = HmacSha256(prk, info_plus_counter);
    SecureZeroMemory(prk.data(), prk.size());
    if (okm.size() > len)
        okm.resize(len);
    return okm;
}

} // namespace


std::string GetMachineHWID()
{
    // 1. MachineGuid del Registro
    char guid[256]{};
    DWORD size = sizeof(guid);
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        RegQueryValueExA(hKey, "MachineGuid", nullptr, nullptr, reinterpret_cast<LPBYTE>(guid), &size);
        RegCloseKey(hKey);
    }

    // 2. Número de Serie del Volumen de la unidad de sistema
    char sys_dir[MAX_PATH]{};
    GetSystemDirectoryA(sys_dir, sizeof(sys_dir));
    char root_path[4] = "C:\\";
    if (sys_dir[0] >= 'A' && sys_dir[0] <= 'Z')
    {
        root_path[0] = sys_dir[0];
    }
    DWORD vol_serial = 0;
    GetVolumeInformationA(root_path, nullptr, 0, &vol_serial, nullptr, nullptr, nullptr, 0);

    // 3. CPUID de hardware real (Firma del procesador y características)
    int cpu_info[4]{};
    __cpuid(cpu_info, 0);
    int cpu_feat[4]{};
    __cpuid(cpu_feat, 1);

    char raw_hw_data[512]{};
    std::snprintf(raw_hw_data, sizeof(raw_hw_data),
                  "%s;%08X;%08X-%08X-%08X-%08X;%08X-%08X",
                  guid, vol_serial,
                  cpu_info[0], cpu_info[1], cpu_info[2], cpu_info[3],
                  cpu_feat[0], cpu_feat[3]);

    // 4. Hash SHA-256 inmutable de 64 caracteres
    const std::string sha256_hwid = ComputeSha256Hex(raw_hw_data);
    if (!sha256_hwid.empty())
    {
        return sha256_hwid;
    }

    // Fallback de contingencia si falla CNG
    return std::string(raw_hw_data);
}

bool RequestAutoLogin(
    const std::string& server_host,
    int port,
    const std::string& hwid,
    std::string& out_download_token,
    std::string& out_message)
{
    ScopedHInternet session(WinHttpOpen(L"WrathAuth/3.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { out_message = "Error al inicializar WinHTTP."; return false; }

    ScopedHInternet connect(WinHttpConnect(session.get(), ToWide(server_host).c_str(), static_cast<INTERNET_PORT>(port), 0));
    if (!connect) { out_message = "Servidor de autenticacion no disponible."; return false; }

    const DWORD flags = (port == 443) ? WINHTTP_FLAG_SECURE : 0;
    ScopedHInternet request(WinHttpOpenRequest(connect.get(), L"POST", ToWide(CRYPT_STR("/api/v1/auth/auto-login")).c_str(), nullptr,
                                               nullptr, nullptr, flags));
    if (!request) { out_message = "Error al crear la peticion HTTP."; return false; }

    std::string json_body = "{\"hwid\":\"" + hwid + "\"}";
    std::wstring headers = ToWide(CRYPT_STR("Content-Type: application/json\r\n"));

    BOOL sent = WinHttpSendRequest(request.get(), headers.c_str(), (DWORD)headers.length(),
                                   (LPVOID)json_body.c_str(), (DWORD)json_body.length(),
                                   (DWORD)json_body.length(), 0);

    if (!sent || !WinHttpReceiveResponse(request.get(), nullptr))
    {
        out_message = "Fallo de conexion con el servidor.";
        return false;
    }

    if (!ValidateServerPin(request.get()))
    {
        out_message = "Certificado del servidor no valido (pin invalido).";
        return false;
    }

    DWORD statusCode = 0;
    DWORD dwSize = sizeof(statusCode);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

    std::string response_body;
    DWORD bytes_avail = 0;
    while (WinHttpQueryDataAvailable(request.get(), &bytes_avail) && bytes_avail > 0)
    {
        std::vector<char> buf(bytes_avail);
        DWORD bytes_read = 0;
        if (WinHttpReadData(request.get(), buf.data(), bytes_avail, &bytes_read))
            response_body.append(buf.data(), bytes_read);
    }

    if (statusCode == 200)
    {
        out_download_token = ExtractJsonField(response_body, "download_token");
        out_message = ExtractJsonField(response_body, "message");
        return !out_download_token.empty();
    }
    else
    {
        out_message = ExtractJsonField(response_body, "detail");
        return false;
    }
}

bool RequestDiscordAuth(
    const std::string& server_host,
    int port,
    const std::string& discord_code,
    const std::string& hwid,
    std::string& out_session_id,
    std::string& out_message)
{
    ScopedHInternet session(WinHttpOpen(L"WrathAuth/3.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { out_message = "Error al inicializar WinHTTP."; return false; }

    ScopedHInternet connect(WinHttpConnect(session.get(), ToWide(server_host).c_str(), static_cast<INTERNET_PORT>(port), 0));
    if (!connect) { out_message = "No se pudo conectar al servidor de autenticacion."; return false; }

    const DWORD flags = (port == 443) ? WINHTTP_FLAG_SECURE : 0;
    ScopedHInternet request(WinHttpOpenRequest(connect.get(), L"POST", ToWide(CRYPT_STR("/api/v1/auth/request")).c_str(), nullptr,
                                               nullptr, nullptr, flags));
    if (!request) { out_message = "Error al crear la peticion HTTP."; return false; }

    char pc_name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD pc_size = sizeof(pc_name);
    GetComputerNameA(pc_name, &pc_size);

    std::string json_body = "{\"code\":\"" + discord_code + "\",\"hwid\":\"" + hwid + "\",\"pc_name\":\"" + std::string(pc_name) + "\"}";
    std::wstring headers = ToWide(CRYPT_STR("Content-Type: application/json\r\n"));

    BOOL sent = WinHttpSendRequest(request.get(), headers.c_str(), (DWORD)headers.length(),
                                   (LPVOID)json_body.c_str(), (DWORD)json_body.length(),
                                   (DWORD)json_body.length(), 0);

    if (!sent || !WinHttpReceiveResponse(request.get(), nullptr))
    {
        out_message = "Fallo de conexion o servidor apagado.";
        return false;
    }

    if (!ValidateServerPin(request.get()))
    {
        out_message = "Certificado del servidor no valido (pin invalido).";
        return false;
    }

    DWORD statusCode = 0;
    DWORD dwSize = sizeof(statusCode);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

    std::string response_body;
    DWORD bytes_avail = 0;
    while (WinHttpQueryDataAvailable(request.get(), &bytes_avail) && bytes_avail > 0)
    {
        std::vector<char> buf(bytes_avail);
        DWORD bytes_read = 0;
        if (WinHttpReadData(request.get(), buf.data(), bytes_avail, &bytes_read))
            response_body.append(buf.data(), bytes_read);
    }

    if (statusCode == 200)
    {
        out_session_id = ExtractJsonField(response_body, "session_id");
        out_message = ExtractJsonField(response_body, "message");
        return true;
    }
    else
    {
        out_message = ExtractJsonField(response_body, "detail");
        if (out_message.empty())
            out_message = "Error (" + std::to_string(statusCode) + ")";
        return false;
    }
}

SessionState PollDiscordSession(
    const std::string& server_host,
    int port,
    const std::string& session_id,
    std::string& out_download_token)
{
    ScopedHInternet session(WinHttpOpen(L"WrathAuth/3.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return SessionState::Error;

    ScopedHInternet connect(WinHttpConnect(session.get(), ToWide(server_host).c_str(), static_cast<INTERNET_PORT>(port), 0));
    if (!connect) return SessionState::Error;

    const std::wstring path = ToWide(CRYPT_STR("/api/v1/auth/poll/")) + ToWide(session_id);
    const DWORD flags = (port == 443) ? WINHTTP_FLAG_SECURE : 0;
    ScopedHInternet request(WinHttpOpenRequest(connect.get(), L"GET", path.c_str(), nullptr,
                                               nullptr, nullptr, flags));
    if (!request) return SessionState::Error;

    if (WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(request.get(), nullptr))
    {
        if (!ValidateServerPin(request.get()))
            return SessionState::Error;

        std::string response_body;
        DWORD bytes_avail = 0;
        while (WinHttpQueryDataAvailable(request.get(), &bytes_avail) && bytes_avail > 0)
        {
            std::vector<char> buf(bytes_avail);
            DWORD bytes_read = 0;
            if (WinHttpReadData(request.get(), buf.data(), bytes_avail, &bytes_read))
                response_body.append(buf.data(), bytes_read);
        }

        std::string status = ExtractJsonField(response_body, "status");
        if (status == "approved")
        {
            out_download_token = ExtractJsonField(response_body, "download_token");
            return SessionState::Approved;
        }
        else if (status == "rejected")
        {
            return SessionState::Rejected;
        }
        else if (status == "expired")
        {
            return SessionState::Expired;
        }
        return SessionState::Pending;
    }

    return SessionState::Error;
}

bool FetchAndDecryptPayload(
    const std::string& server_host,
    int port,
    const std::string& download_token,
    const std::string& hwid,
    std::vector<uint8_t>& out_dll_bytes,
    std::string& out_error,
    std::string* out_heartbeat_token,
    std::string* out_session_id,
    std::string* out_client_nonce)
{

    // 1. Generar par de claves efímero ECDH P-256 del cliente
    BCRYPT_ALG_HANDLE hEcdhAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hEcdhAlg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0);
    if (!NT_SUCCESS(status))
    {
        out_error = "Fallo al inicializar proveedor ECDH P-256.";
        return false;
    }

    BCRYPT_KEY_HANDLE hClientKey = nullptr;
    status = BCryptGenerateKeyPair(hEcdhAlg, &hClientKey, 256, 0);
    if (!NT_SUCCESS(status) || !NT_SUCCESS(BCryptFinalizeKeyPair(hClientKey, 0)))
    {
        if (hClientKey) BCryptDestroyKey(hClientKey);
        BCryptCloseAlgorithmProvider(hEcdhAlg, 0);
        out_error = "Fallo al generar clave efímera ECDH.";
        return false;
    }

    // Exportar clave pública efímera del cliente (64 bytes X || Y)
    DWORD cbPubBlob = 0;
    BCryptExportKey(hClientKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &cbPubBlob, 0);
    std::vector<uint8_t> clientPubBlob(cbPubBlob);
    BCryptExportKey(hClientKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, clientPubBlob.data(), cbPubBlob, &cbPubBlob, 0);

    const uint8_t* rawClientPub = clientPubBlob.data() + sizeof(BCRYPT_ECCKEY_BLOB);
    const std::string clientPubHex = ToHex(rawClientPub, 64);

    // 2. Conectar al servidor mediante POST
    ScopedHInternet session(WinHttpOpen(L"WrathAuth/3.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
    {
        BCryptDestroyKey(hClientKey);
        BCryptCloseAlgorithmProvider(hEcdhAlg, 0);
        out_error = "Error al inicializar WinHTTP.";
        return false;
    }

    ScopedHInternet connect(WinHttpConnect(session.get(), ToWide(server_host).c_str(), static_cast<INTERNET_PORT>(port), 0));
    if (!connect)
    {
        BCryptDestroyKey(hClientKey);
        BCryptCloseAlgorithmProvider(hEcdhAlg, 0);
        out_error = "No se pudo conectar al endpoint de descarga.";
        return false;
    }

    const std::wstring path = ToWide(CRYPT_STR("/api/v1/payload/download"));
    const DWORD flags = (port == 443) ? WINHTTP_FLAG_SECURE : 0;
    ScopedHInternet request(WinHttpOpenRequest(connect.get(), L"POST", path.c_str(), nullptr,
                                               nullptr, nullptr, flags));
    if (!request)
    {
        BCryptDestroyKey(hClientKey);
        BCryptCloseAlgorithmProvider(hEcdhAlg, 0);
        out_error = "Fallo al crear request de descarga.";
        return false;
    }

    std::string json_body = "{\"download_token\":\"" + download_token + "\",\"hwid\":\"" + hwid + "\",\"client_pubkey\":\"" + clientPubHex + "\"}";
    std::wstring headers = ToWide(CRYPT_STR("Content-Type: application/json\r\n"));

    BOOL sent = WinHttpSendRequest(request.get(), headers.c_str(), (DWORD)headers.length(),
                                   (LPVOID)json_body.c_str(), (DWORD)json_body.length(),
                                   (DWORD)json_body.length(), 0);

    if (!sent || !WinHttpReceiveResponse(request.get(), nullptr))
    {
        BCryptDestroyKey(hClientKey);
        BCryptCloseAlgorithmProvider(hEcdhAlg, 0);
        out_error = "Fallo recibiendo el binario cifrado.";
        return false;
    }

    if (!ValidateServerPin(request.get()))
    {
        BCryptDestroyKey(hClientKey);
        BCryptCloseAlgorithmProvider(hEcdhAlg, 0);
        out_error = "Certificado del servidor no valido (pin invalido).";
        return false;
    }

    DWORD statusCode = 0;
    DWORD dwStatusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwStatusSize, WINHTTP_NO_HEADER_INDEX);

    // Extraer cabeceras de sesión de latido si están presentes
    if (out_heartbeat_token)
    {
        wchar_t hbBuf[256]{};
        DWORD dwHbSize = sizeof(hbBuf);
        if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CUSTOM, L"X-Heartbeat-Token",
                                hbBuf, &dwHbSize, WINHTTP_NO_HEADER_INDEX))
        {
            char narrowBuf[256]{};
            WideCharToMultiByte(CP_UTF8, 0, hbBuf, -1, narrowBuf, sizeof(narrowBuf) - 1, nullptr, nullptr);
            *out_heartbeat_token = narrowBuf;
        }
    }
    if (out_session_id)
    {
        wchar_t sidBuf[256]{};
        DWORD dwSidSize = sizeof(sidBuf);
        if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CUSTOM, L"X-Session-ID",
                                sidBuf, &dwSidSize, WINHTTP_NO_HEADER_INDEX))
        {
            char narrowBuf[256]{};
            WideCharToMultiByte(CP_UTF8, 0, sidBuf, -1, narrowBuf, sizeof(narrowBuf) - 1, nullptr, nullptr);
            *out_session_id = narrowBuf;
        }
    }

    std::vector<uint8_t> response_body;
    DWORD bytes_avail = 0;
    while (WinHttpQueryDataAvailable(request.get(), &bytes_avail) && bytes_avail > 0)
    {
        size_t prev_size = response_body.size();
        response_body.resize(prev_size + bytes_avail);
        DWORD bytes_read = 0;
        if (WinHttpReadData(request.get(), response_body.data() + prev_size, bytes_avail, &bytes_read))
            response_body.resize(prev_size + bytes_read);
    }

    if (statusCode != 200)
    {
        BCryptDestroyKey(hClientKey);
        BCryptCloseAlgorithmProvider(hEcdhAlg, 0);
        std::string err_str(response_body.begin(), response_body.end());
        out_error = ExtractJsonField(err_str, "detail");
        if (out_error.empty())
            out_error = "Error servidor (" + std::to_string(statusCode) + ")";
        return false;
    }

    // Estructura de respuesta:
    // [64 bytes SERVER PUBKEY (X || Y)] + [12 bytes NONCE] + [CIPHERTEXT] + [16 bytes GCM TAG]
    constexpr size_t kServerPubKeySize = 64;
    constexpr size_t kNonceSize = 12;
    constexpr size_t kTagSize = 16;
    if (response_body.size() <= kServerPubKeySize + kNonceSize + kTagSize)
    {
        BCryptDestroyKey(hClientKey);
        BCryptCloseAlgorithmProvider(hEcdhAlg, 0);
        out_error = "Paquete cifrado corrupto recibido.";
        return false;
    }

    const uint8_t* serverPubRaw = response_body.data();
    const uint8_t* nonce = response_body.data() + kServerPubKeySize;
    const size_t ciphertext_len = response_body.size() - kServerPubKeySize - kNonceSize - kTagSize;
    uint8_t* ciphertext = response_body.data() + kServerPubKeySize + kNonceSize;
    uint8_t* tag = response_body.data() + kServerPubKeySize + kNonceSize + ciphertext_len;

    // 3. Importar clave pública del servidor
    std::vector<uint8_t> serverPubBlob(sizeof(BCRYPT_ECCKEY_BLOB) + kServerPubKeySize);
    auto* pServerHdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(serverPubBlob.data());
    pServerHdr->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    pServerHdr->cbKey = 32;
    std::memcpy(serverPubBlob.data() + sizeof(BCRYPT_ECCKEY_BLOB), serverPubRaw, kServerPubKeySize);

    BCRYPT_KEY_HANDLE hServerKey = nullptr;
    status = BCryptImportKeyPair(hEcdhAlg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &hServerKey,
                                 serverPubBlob.data(), static_cast<ULONG>(serverPubBlob.size()), 0);
    if (!NT_SUCCESS(status))
    {
        BCryptDestroyKey(hClientKey);
        BCryptCloseAlgorithmProvider(hEcdhAlg, 0);
        out_error = "Fallo al importar clave pública efímera del servidor.";
        return false;
    }

    // 4. Secret Agreement (Diffie-Hellman)
    BCRYPT_SECRET_HANDLE hSecret = nullptr;
    status = BCryptSecretAgreement(hClientKey, hServerKey, &hSecret, 0);
    if (!NT_SUCCESS(status))
    {
        BCryptDestroyKey(hServerKey);
        BCryptDestroyKey(hClientKey);
        BCryptCloseAlgorithmProvider(hEcdhAlg, 0);
        out_error = "Fallo en acuerdo de secretos ECDH.";
        return false;
    }

    DWORD cbSecret = 0;
    BCryptDeriveKey(hSecret, BCRYPT_KDF_RAW_SECRET, nullptr, nullptr, 0, &cbSecret, 0);
    std::vector<uint8_t> rawSecret(cbSecret);
    BCryptDeriveKey(hSecret, BCRYPT_KDF_RAW_SECRET, nullptr, rawSecret.data(), cbSecret, &cbSecret, 0);

    BCryptDestroySecret(hSecret);
    BCryptDestroyKey(hServerKey);
    BCryptDestroyKey(hClientKey);
    BCryptCloseAlgorithmProvider(hEcdhAlg, 0);

    // Invertir a big-endian (RFC/Python compat)
    std::reverse(rawSecret.begin(), rawSecret.end());

    // 5. Derivar clave de sesión AES-256 usando HKDF-SHA256
    const std::string salt_str = hwid + download_token;
    std::vector<uint8_t> salt(salt_str.begin(), salt_str.end());
    const std::string info_str = "wrath-payload-key-derivation";
    std::vector<uint8_t> info(info_str.begin(), info_str.end());

    std::vector<uint8_t> session_key = HkdfSha256(rawSecret, salt, info, 32);

    // Limpieza segura inmediata del secreto crudo
    SecureZeroMemory(rawSecret.data(), rawSecret.size());

    // 6. Descifrado en RAM con BCrypt AES-GCM
    BCRYPT_ALG_HANDLE hAesAlg = nullptr;
    status = BCryptOpenAlgorithmProvider(&hAesAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!NT_SUCCESS(status))
    {
        SecureZeroMemory(session_key.data(), session_key.size());
        out_error = "Error inicializando BCrypt AES.";
        return false;
    }

    status = BCryptSetProperty(hAesAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!NT_SUCCESS(status))
    {
        SecureZeroMemory(session_key.data(), session_key.size());
        BCryptCloseAlgorithmProvider(hAesAlg, 0);
        out_error = "Modo GCM no soportado.";
        return false;
    }

    BCRYPT_KEY_HANDLE hAesKey = nullptr;
    status = BCryptGenerateSymmetricKey(hAesAlg, &hAesKey, nullptr, 0, (PUCHAR)session_key.data(), (ULONG)session_key.size(), 0);

    // Limpieza segura de la clave en memoria tras importarla a CNG
    SecureZeroMemory(session_key.data(), session_key.size());

    if (!NT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(hAesAlg, 0);
        out_error = "Clave de descifrado inválida.";
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(nonce);
    authInfo.cbNonce = kNonceSize;
    authInfo.pbAuthData = (PUCHAR)hwid.data();
    authInfo.cbAuthData = (ULONG)hwid.size();
    authInfo.pbTag = tag;
    authInfo.cbTag = kTagSize;

    out_dll_bytes.resize(ciphertext_len);
    ULONG decrypted_bytes = 0;
    status = BCryptDecrypt(hAesKey, ciphertext, (ULONG)ciphertext_len, &authInfo, nullptr, 0,
                           out_dll_bytes.data(), (ULONG)out_dll_bytes.size(), &decrypted_bytes, 0);

    BCryptDestroyKey(hAesKey);
    BCryptCloseAlgorithmProvider(hAesAlg, 0);

    if (!NT_SUCCESS(status))
    {
        out_dll_bytes.clear();
        out_error = "Fallo de integridad criptográfica (GCM Tag Mismatch).";
        return false;
    }

    out_dll_bytes.resize(decrypted_bytes);

    // Nonce vinculado al cliente: SHA-256 de su clave pública efímera ECDH.
    // El servidor valida que el heartbeat llegue desde el mismo par criptografico.
    if (out_client_nonce)
    {
        std::string rawClientPubStr(reinterpret_cast<const char*>(rawClientPub), 64);
        *out_client_nonce = ComputeSha256Hex(rawClientPubStr);
    }

    return true;
}

} // namespace wrath::auth

