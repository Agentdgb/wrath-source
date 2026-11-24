#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#include <d3d11.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#include <dwmapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <fstream>
#include <iostream>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "misc/freetype/imgui_freetype.h"

#include "launcher_injector.h"
#include "auth/AuthClient.h"
#include "auth/ObfuscatedString.h"
#include "shared/stealth/SessionStealth.h"
#include "trace/teardown.h"
#include "trace/snapshot.h"
#include "loader/Paths.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

namespace
{
constexpr float kDesignWidth = 520.0f;
constexpr float kDesignHeight = 370.0f;

namespace Palette
{
constexpr unsigned int panel = 0x0c0b10;
constexpr unsigned int border = 0x1f1c26;
constexpr unsigned int input_bg = 0x100e14;
constexpr unsigned int input_hover = 0x141118;
constexpr unsigned int input_active = 0x16131c;
constexpr unsigned int input_border = 0x2c1418;
constexpr unsigned int crimson = 0xe61e28;
constexpr unsigned int crimson_dark = 0x8a0a10;
constexpr unsigned int crimson_hover = 0xff2d38;
constexpr unsigned int icon_red = 0xff2230;
constexpr unsigned int text_light = 0xf2f1f5;
constexpr unsigned int text_dim = 0x71717a;
} // namespace Palette

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_device_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_main_render_target_view = nullptr;
HWND g_main_window = nullptr;
UINT g_resize_width = 0;
UINT g_resize_height = 0;
float g_dpi_scale = 1.0f;

ID3D11ShaderResourceView* g_new_logo = nullptr;

// DirectX 11 Background Wave Shader Pipeline
ID3D11VertexShader* g_wave_vs = nullptr;
ID3D11PixelShader* g_wave_ps = nullptr;
ID3D11Buffer* g_wave_cb = nullptr;
ID3D11Texture2D* g_wave_texture = nullptr;
ID3D11RenderTargetView* g_wave_rtv = nullptr;
ID3D11ShaderResourceView* g_wave_srv = nullptr;

enum class FontFamily { Default, SfPro, Inter };

struct FontBook
{
    ImFont* regular = nullptr;
    ImFont* semibold = nullptr;
    std::array<ImFont*, 65> text_sizes{};
    std::array<ImFont*, 65> sf_pro_sizes{};
    std::array<ImFont*, 65> inter_sizes{};
};

FontBook g_fonts;

inline ImU32 Color(unsigned int rgb, int alpha = 255)
{
    return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, alpha);
}

inline ImVec4 Color4(unsigned int rgb, float alpha = 1.0f)
{
    return ImVec4(
        static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
        static_cast<float>(rgb & 0xff) / 255.0f,
        alpha);
}

struct Interaction
{
    bool clicked = false;
    bool hovered = false;
    bool active = false;
};

struct Canvas
{
    ImVec2 origin{};
    float scale = 1.0f;
    ImDrawList* draw = nullptr;

    ImVec2 Point(float x, float y) const
    {
        return ImVec2(origin.x + x * scale, origin.y + y * scale);
    }

    ImVec2 Size(float x, float y) const
    {
        return ImVec2(x * scale, y * scale);
    }

    float Px(float value) const
    {
        return value * scale;
    }

    void Rect(float x, float y, float width, float height, ImU32 color, float rounding = 0.0f,
              ImDrawFlags flags = 0) const
    {
        draw->AddRectFilled(Point(x, y), Point(x + width, y + height), color, Px(rounding), flags);
    }

    void StrokeRect(float x, float y, float width, float height, ImU32 color, float rounding = 0.0f,
                    float thickness = 1.0f) const
    {
        draw->AddRect(Point(x, y), Point(x + width, y + height), color, Px(rounding), 0, Px(thickness));
    }

    void Line(float x1, float y1, float x2, float y2, ImU32 color, float thickness = 1.0f) const
    {
        draw->AddLine(Point(x1, y1), Point(x2, y2), color, Px(thickness));
    }

    void Circle(float x, float y, float radius, ImU32 color, float thickness = 1.0f) const
    {
        draw->AddCircle(Point(x, y), Px(radius), color, 0, Px(thickness));
    }

    void FilledCircle(float x, float y, float radius, ImU32 color) const
    {
        draw->AddCircleFilled(Point(x, y), Px(radius), color);
    }

    ImFont* ResolveFont(int pixels, bool semibold) const
    {
        ImFont* font = nullptr;
        if (pixels >= 0 && pixels < static_cast<int>(g_fonts.text_sizes.size()))
        {
            font = semibold ? g_fonts.sf_pro_sizes[pixels] : g_fonts.text_sizes[pixels];
        }
        if (!font) font = semibold ? g_fonts.semibold : g_fonts.regular;
        if (!font) font = g_fonts.regular;
        return font;
    }

    void Text(float x, float y, std::string_view value, ImU32 color, float size = 13.0f,
              bool semibold = false) const
    {
        const int pixels = std::clamp(static_cast<int>(std::round(Px(size))), 8, 64);
        ImFont* font = ResolveFont(pixels, semibold);
        const ImVec2 position = Point(x, y - 1.0f);
        draw->AddText(font, static_cast<float>(pixels), ImVec2(std::round(position.x), std::round(position.y)),
                      color, value.data(), value.data() + value.size());
    }

    float TextWidth(std::string_view value, float size = 13.0f, bool semibold = false) const
    {
        const int pixels = std::clamp(static_cast<int>(std::round(Px(size))), 8, 64);
        ImFont* font = ResolveFont(pixels, semibold);
        return font->CalcTextSizeA(static_cast<float>(pixels), FLT_MAX, 0.0f, value.data(), value.data() + value.size()).x;
    }

    Interaction Hit(const char* id, float x, float y, float width, float height) const
    {
        ImGui::SetCursorScreenPos(Point(x, y));
        ImGui::InvisibleButton(id, Size(width, height));
        return {ImGui::IsItemClicked(ImGuiMouseButton_Left), ImGui::IsItemHovered(), ImGui::IsItemActive()};
    }
};

void DrawNewLogo(const Canvas& canvas, float center_x, float center_y, float size = 32.0f)
{
    if (g_new_logo)
    {
        const float half_w = size * 0.5f;
        const float half_h = size * 0.5f;
        ImVec2 p_min = canvas.Point(center_x - half_w, center_y - half_h);
        ImVec2 p_max = canvas.Point(center_x + half_w, center_y + half_h);
        p_min.x = std::round(p_min.x);
        p_min.y = std::round(p_min.y);
        p_max.x = std::round(p_max.x);
        p_max.y = std::round(p_max.y);

        canvas.draw->AddImage((ImTextureID)(intptr_t)g_new_logo, p_min, p_max,
            ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255));
    }
}

enum class LoaderStage
{
    Login,
    Injecting
};

enum class Language
{
    English = 0,
    Spanish = 1
};

enum class InjectStatusCode
{
    Idle,
    Scanning,
    Detected,
    InjectedSuccess,
    InjectFailed,
    AlreadyActive
};

struct LocalizedStrings
{
    const char* login_label;
    const char* password_label;
    const char* enter_btn;
    const char* connecting;
    const char* loading_title;
};

inline const LocalizedStrings& GetStrings(Language lang)
{
    static const LocalizedStrings en = {
        "Enter Password (or press Enter)",
        "",
        "Enter",
        "Loading...",
        "Wrath is Loading"
    };

    static const LocalizedStrings es = {
        "Contraseña (o pulsa Entrar)",
        "",
        "Entrar",
        "Cargando...",
        "Wrath está Cargando"
    };

    return (lang == Language::Spanish) ? es : en;
}

inline const char* GetStatusString(InjectStatusCode code, Language lang)
{
    if (lang == Language::Spanish)
    {
        switch (code)
        {
        case InjectStatusCode::Scanning: return "Buscando Minecraft (javaw.exe)...";
        case InjectStatusCode::Detected: return "Minecraft detectado - Inyectando carga...";
        case InjectStatusCode::InjectedSuccess: return "¡Inyectado exitosamente en Minecraft!";
        case InjectStatusCode::InjectFailed: return "Error al inyectar DLL.";
        case InjectStatusCode::AlreadyActive: return "Wrath ya está activo en Minecraft.";
        default: return "Esperando proceso destino...";
        }
    }
    else
    {
        switch (code)
        {
        case InjectStatusCode::Scanning: return "Scanning for Minecraft (javaw.exe)...";
        case InjectStatusCode::Detected: return "Minecraft detected - Injecting payload...";
        case InjectStatusCode::InjectedSuccess: return "Injected successfully into Minecraft!";
        case InjectStatusCode::InjectFailed: return "Failed to inject DLL.";
        case InjectStatusCode::AlreadyActive: return "Wrath is already active in Minecraft.";
        default: return "Waiting for target process...";
        }
    }
}

struct AppState
{
    // ── Subsistema 1: Hilo Principal (Render DX11 & Interfaz) ────────────────
    alignas(64) std::atomic<LoaderStage> loader_stage{LoaderStage::Login};
    std::atomic<bool> is_logged_in{false};
    std::atomic<Language> current_lang{Language::English};
    std::atomic<InjectStatusCode> inject_code{InjectStatusCode::Scanning};

    char username_buf[64] = "";
    char password_buf[64] = "";
    bool remember_login = true;
    bool show_password = false;

    // ── Subsistema 2: Hilo de Autenticación de Red (Discord Handshake) ───────
    alignas(64) std::atomic<bool> is_authenticating{false};
    float auth_timer = 0.0f;
    std::string auth_error_message;
    std::string auth_status_message;
    std::mutex auth_msg_mutex;

    std::string server_host = "127.0.0.1";
    int server_port = 8000;
    bool dev_bypass = false;

    // Injection strategy overrides (plan: seleccionable).
    std::string inject_strategy = "auto";
    bool no_unlink = false;
    bool no_wipe_headers = false;
    bool no_reprotect = false;
    bool no_zero_scratch = false;
    bool no_hide_thread = false;

    void SetAuthError(std::string msg)
    {
        std::lock_guard lock(auth_msg_mutex);
        auth_error_message = std::move(msg);
    }

    std::string GetAuthError()
    {
        std::lock_guard lock(auth_msg_mutex);
        return auth_error_message;
    }

    void SetAuthStatus(std::string msg)
    {
        std::lock_guard lock(auth_msg_mutex);
        auth_status_message = std::move(msg);
    }

    std::string GetAuthStatus()
    {
        std::lock_guard lock(auth_msg_mutex);
        return auth_status_message;
    }

    void ClearAuthMessages()
    {
        std::lock_guard lock(auth_msg_mutex);
        auth_error_message.clear();
        auth_status_message.clear();
    }

    std::vector<uint8_t> in_memory_payload;
    std::mutex auth_payload_mutex;
    std::string session_heartbeat_token;
    std::string session_client_nonce;
    std::string session_id;

    // ── Subsistema 3: Hilo de Escaneo de Procesos e Inyección (Hot State) ────
    alignas(64) std::atomic<bool> is_injecting{false};
    std::atomic<float> inject_progress{0.0f};
    std::atomic<bool> is_target_found{false};
    std::atomic<bool> is_already_injected{false};
    std::atomic<bool> stealth_thread{true};
    std::atomic<DWORD> detected_pid{0};
    bool auto_inject = false;
    bool cli_mode = false;
    bool self_delete = false;
    std::string inject_status = "Waiting for target process...";
    std::wstring detected_name;
    std::wstring detected_title;

    // Interactive background wave animation state
    float wave_time = 0.0f;
    ImVec2 smooth_mouse{260.0f, 185.0f};
    ImVec2 last_mouse{260.0f, 185.0f};
    float mouse_speed = 0.0f;

    // Interactive language menu state
    bool is_lang_menu_open = false;
    float lang_menu_anim = 0.0f;
    float hover_anim_en = 0.0f;
    float hover_anim_es = 0.0f;

    std::vector<std::string> console_logs;
    std::mutex log_mutex;
    std::mutex status_mutex;

    void SetInjectionStatus(std::string status) {
        std::lock_guard lock(status_mutex);
        inject_status = std::move(status);
    }

    void AddLog(const std::string& msg)
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char prefix[32]{};
        std::snprintf(prefix, sizeof(prefix), "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
        std::lock_guard<std::mutex> lock(log_mutex);
        console_logs.push_back(std::string(prefix) + msg);
        if (console_logs.size() > 50)
            console_logs.erase(console_logs.begin());
    }
};

AppState g_state;

std::string ExecutableAssetPath(const char* filename)
{
    std::array<char, MAX_PATH> executable_path{};
    const DWORD length = GetModuleFileNameA(nullptr, executable_path.data(),
                                            static_cast<DWORD>(executable_path.size()));
    std::string directory(executable_path.data(), length);
    const size_t separator = directory.find_last_of("\\/");
    if (separator != std::string::npos)
        directory.resize(separator + 1);
    else
        directory.clear();
    return directory + "assets\\" + filename;
}

std::string ExecutableDirFilePath(const char* filename)
{
    std::array<char, MAX_PATH> executable_path{};
    const DWORD length = GetModuleFileNameA(nullptr, executable_path.data(),
                                            static_cast<DWORD>(executable_path.size()));
    std::string directory(executable_path.data(), length);
    const size_t separator = directory.find_last_of("\\/");
    if (separator != std::string::npos)
        directory.resize(separator + 1);
    else
        directory.clear();
    return directory + filename;
}

void LoadServerConfigFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line))
    {
        std::string_view sv(line);
        while (!sv.empty() && (sv.back() == '\r' || sv.back() == '\n' || sv.back() == ' '))
            sv.remove_suffix(1);
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
            sv.remove_prefix(1);

        if (sv.empty() || sv.front() == '#' || sv.front() == ';' || sv.front() == '[')
            continue;

        const size_t eq_pos = sv.find('=');
        if (eq_pos == std::string_view::npos)
            continue;

        const std::string_view key = sv.substr(0, eq_pos);
        const std::string_view val = sv.substr(eq_pos + 1);

        if (key == "host" || key == "server_host" || key == "server")
        {
            if (!val.empty())
                g_state.server_host = std::string(val);
        }
        else if (key == "port" || key == "server_port")
        {
            try {
                int p = std::stoi(std::string(val));
                if (p > 0 && p <= 65535) g_state.server_port = p;
            } catch (...) {}
        }
    }
}

bool LoadLocalPayload(std::vector<uint8_t>& out_bytes)
{
    const std::wstring path = wrath::loader::resolve_payload();
    if (path.empty())
        return false;
    out_bytes = wrath::loader::read_payload(path);
    return out_bytes.size() > 0x1000;
}

inline bool LoadLocalPayloadIntoMemory()
{
    std::wstring local_dll;
    if (!launcher::ResolvePayloadDllPath(local_dll))
        return false;

    HANDLE hFile = CreateFileW(local_dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD size = GetFileSize(hFile, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0)
    {
        CloseHandle(hFile);
        return false;
    }

    std::vector<uint8_t> bytes(size);
    DWORD bytes_read = 0;
    if (!ReadFile(hFile, bytes.data(), size, &bytes_read, nullptr) || bytes_read != size)
    {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    {
        std::lock_guard lock(g_state.auth_payload_mutex);
        g_state.in_memory_payload = std::move(bytes);
    }
    return true;
}

inline void ActivateBypassDirectly()
{
    g_state.dev_bypass = true;
    g_state.is_logged_in.store(true);
    g_state.loader_stage.store(LoaderStage::Injecting);
    g_state.inject_progress.store(0.15f);
    g_state.inject_code.store(InjectStatusCode::Scanning);
    g_state.AddLog("[LOCAL] Sesion autorizada. Escaneando Minecraft 1.21...");
}

void ParseCommandLineOverrides(PWSTR cmdLine)
{
    if (!cmdLine || !cmdLine[0]) return;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    if (!argv) return;

    for (int i = 0; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (arg == L"--server" || arg == L"-server" || arg == L"--host" || arg == L"-host")
        {
            if (i + 1 < argc)
            {
                std::wstring val = argv[++i];
                char buf[256]{};
                WideCharToMultiByte(CP_UTF8, 0, val.c_str(), -1, buf, sizeof(buf) - 1, nullptr, nullptr);
                if (buf[0]) g_state.server_host = buf;
            }
        }
        else if (arg == L"--port" || arg == L"-port")
        {
            if (i + 1 < argc)
            {
                std::wstring val = argv[++i];
                try {
                    int p = std::stoi(val);
                    if (p > 0 && p <= 65535) g_state.server_port = p;
                } catch (...) {}
            }
        }
        else if (arg == L"--bypass" || arg == L"-bypass" || arg == L"--dev" || arg == L"-dev" || arg == L"--owner" || arg == L"-owner")
        {
            g_state.dev_bypass = true;
        }
        else if (arg == L"--cli" || arg == L"-cli" || arg == L"--silent" || arg == L"-silent" || arg == L"--headless" || arg == L"-headless" || arg == L"--inject" || arg == L"-inject")
        {
            g_state.cli_mode = true;
            g_state.dev_bypass = true;
        }
        else if (arg == L"--clean" || arg == L"-clean" || arg == L"--self-delete" || arg == L"-self-delete")
        {
            g_state.self_delete = true;
        }
        else if (arg == L"--strategy" || arg == L"--strategy=" || arg == L"-strategy")
        {
            std::wstring val;
            if (arg.find(L"=") != std::wstring::npos) val = arg.substr(arg.find(L"=") + 1);
            else if (i + 1 < argc) val = argv[++i];
            char buf[128]{};
            WideCharToMultiByte(CP_UTF8, 0, val.c_str(), -1, buf, sizeof(buf) - 1, nullptr, nullptr);
            if (buf[0]) g_state.inject_strategy = buf;
        }
        else if (arg == L"--no-unlink")        g_state.no_unlink = true;
        else if (arg == L"--no-wipe-headers")  g_state.no_wipe_headers = true;
        else if (arg == L"--no-reprotect")     g_state.no_reprotect = true;
        else if (arg == L"--no-zero-scratch")  g_state.no_zero_scratch = true;
        else if (arg == L"--no-hide-thread")   g_state.no_hide_thread = true;
    }
    LocalFree(argv);
}

void SaveLoaderConfig()
{
    const std::string path = ExecutableAssetPath("loader_auth.ini");
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return;

    file << "[LoaderAuth]\n"
         << "username=" << g_state.username_buf << "\n"
         << "remember=" << (g_state.remember_login ? 1 : 0) << "\n"
         << "auto_inject=" << (g_state.auto_inject ? 1 : 0) << "\n"
         << "stealth=" << (g_state.stealth_thread ? 1 : 0) << "\n"
         << "language=" << ((g_state.current_lang.load() == Language::Spanish) ? "es" : "en") << "\n"
         << "server_host=" << g_state.server_host << "\n"
         << "server_port=" << g_state.server_port << "\n";
}

void LoadLoaderConfig()
{
    // 1. Try server.cfg from executable root directory and from assets
    LoadServerConfigFile(ExecutableDirFilePath("server.cfg"));
    LoadServerConfigFile(ExecutableAssetPath("server.cfg"));

    // 2. Load loader_auth.ini
    const std::string path = ExecutableAssetPath("loader_auth.ini");
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line))
    {
        std::string_view sv(line);
        while (!sv.empty() && (sv.back() == '\r' || sv.back() == '\n' || sv.back() == ' '))
            sv.remove_suffix(1);
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
            sv.remove_prefix(1);

        if (sv.empty() || sv.front() == '[' || sv.front() == '#' || sv.front() == ';')
            continue;

        const size_t eq_pos = sv.find('=');
        if (eq_pos == std::string_view::npos)
            continue;

        const std::string_view key = sv.substr(0, eq_pos);
        const std::string_view val = sv.substr(eq_pos + 1);

        if (key == "username")
        {
            const size_t copy_len = (std::min)(val.size(), sizeof(g_state.username_buf) - 1);
            std::memcpy(g_state.username_buf, val.data(), copy_len);
            g_state.username_buf[copy_len] = '\0';
        }
        else if (key == "remember")
        {
            g_state.remember_login = (val == "1" || val == "true");
        }
        else if (key == "auto_inject")
        {
            g_state.auto_inject = (val == "1" || val == "true");
        }
        else if (key == "stealth")
        {
            g_state.stealth_thread = (val == "1" || val == "true");
        }
        else if (key == "language")
        {
            if (val == "es" || val == "spanish" || val == "ES")
                g_state.current_lang.store(Language::Spanish);
            else
                g_state.current_lang.store(Language::English);
        }
        else if (key == "server_host" || key == "host")
        {
            if (!val.empty())
                g_state.server_host = std::string(val);
        }
        else if (key == "server_port" || key == "port")
        {
            try {
                int p = std::stoi(std::string(val));
                if (p > 0 && p <= 65535) g_state.server_port = p;
            } catch (...) {}
        }
    }
}


void DrawRoundedGradientH(ImDrawList* draw, ImVec2 p_min, ImVec2 p_max, ImU32 col_left, ImU32 col_right, float rounding)
{
    const int steps = 16;
    const float total_w = p_max.x - p_min.x;
    if (total_w <= 0.0f) return;

    const int r1 = (col_left >> IM_COL32_R_SHIFT) & 0xff;
    const int g1 = (col_left >> IM_COL32_G_SHIFT) & 0xff;
    const int b1 = (col_left >> IM_COL32_B_SHIFT) & 0xff;
    const int a1 = (col_left >> IM_COL32_A_SHIFT) & 0xff;

    const int r2 = (col_right >> IM_COL32_R_SHIFT) & 0xff;
    const int g2 = (col_right >> IM_COL32_G_SHIFT) & 0xff;
    const int b2 = (col_right >> IM_COL32_B_SHIFT) & 0xff;
    const int a2 = (col_right >> IM_COL32_A_SHIFT) & 0xff;

    for (int i = 0; i < steps; ++i)
    {
        const float t1 = static_cast<float>(i) / static_cast<float>(steps);
        const float t2 = static_cast<float>(i + 1) / static_cast<float>(steps);

        const int r = static_cast<int>(r1 + (r2 - r1) * (t1 + t2) * 0.5f);
        const int g = static_cast<int>(g1 + (g2 - g1) * (t1 + t2) * 0.5f);
        const int b = static_cast<int>(b1 + (b2 - b1) * (t1 + t2) * 0.5f);
        const int a = static_cast<int>(a1 + (a2 - a1) * (t1 + t2) * 0.5f);
        const ImU32 col = IM_COL32(r, g, b, a);

        const float x0 = p_min.x + t1 * total_w;
        const float x1 = (i == steps - 1) ? p_max.x : (p_min.x + t2 * total_w + 0.5f);

        ImDrawFlags flags = 0;
        float cur_rounding = 0.0f;
        if (i == 0)
        {
            flags = ImDrawFlags_RoundCornersLeft;
            cur_rounding = rounding;
        }
        else if (i == steps - 1)
        {
            flags = ImDrawFlags_RoundCornersRight;
            cur_rounding = rounding;
        }

        draw->AddRectFilled(ImVec2(x0, p_min.y), ImVec2(x1, p_max.y), col, cur_rounding, flags);
    }
}

void DrawFlag(ImDrawList* draw, ImVec2 pos, ImVec2 size, Language lang)
{
    const float rounding = 2.5f;
    const ImVec2 p_max(pos.x + size.x, pos.y + size.y);

    draw->PushClipRect(pos, p_max, true);

    if (lang == Language::English)
    {
        // 🇬🇧 United Kingdom Flag
        // 1. Blue field
        draw->AddRectFilled(pos, p_max, IM_COL32(1, 33, 105, 255), rounding);

        // 2. White diagonal saltire
        draw->AddLine(pos, p_max, IM_COL32(255, 255, 255, 255), 2.5f);
        draw->AddLine(ImVec2(pos.x + size.x, pos.y), ImVec2(pos.x, pos.y + size.y), IM_COL32(255, 255, 255, 255), 2.5f);

        // 3. Red diagonal saltire
        draw->AddLine(pos, p_max, IM_COL32(200, 16, 46, 255), 1.2f);
        draw->AddLine(ImVec2(pos.x + size.x, pos.y), ImVec2(pos.x, pos.y + size.y), IM_COL32(200, 16, 46, 255), 1.2f);

        // 4. White central cross
        const float mid_x = pos.x + size.x * 0.5f;
        const float mid_y = pos.y + size.y * 0.5f;
        draw->AddRectFilled(ImVec2(mid_x - 2.0f, pos.y), ImVec2(mid_x + 2.0f, p_max.y), IM_COL32_WHITE);
        draw->AddRectFilled(ImVec2(pos.x, mid_y - 2.0f), ImVec2(p_max.x, mid_y + 2.0f), IM_COL32_WHITE);

        // 5. Red central cross
        draw->AddRectFilled(ImVec2(mid_x - 1.0f, pos.y), ImVec2(mid_x + 1.0f, p_max.y), IM_COL32(200, 16, 46, 255));
        draw->AddRectFilled(ImVec2(pos.x, mid_y - 1.0f), ImVec2(p_max.x, mid_y + 1.0f), IM_COL32(200, 16, 46, 255));
    }
    else
    {
        // 🇪🇸 Spain Flag
        // 1. Yellow central band
        draw->AddRectFilled(pos, p_max, IM_COL32(241, 191, 0, 255), rounding);

        // 2. Red top and bottom bands (each 28% of height)
        const float red_h = size.y * 0.28f;
        draw->AddRectFilled(pos, ImVec2(p_max.x, pos.y + red_h), IM_COL32(170, 21, 27, 255));
        draw->AddRectFilled(ImVec2(pos.x, p_max.y - red_h), p_max, IM_COL32(170, 21, 27, 255));

        // 3. Spanish Coat of Arms emblem
        const float crest_x = pos.x + size.x * 0.32f;
        const float crest_y = pos.y + size.y * 0.50f;
        draw->AddRectFilled(ImVec2(crest_x - 1.5f, crest_y - 2.0f), ImVec2(crest_x + 1.5f, crest_y + 2.0f), IM_COL32(170, 21, 27, 255), 1.0f);
        draw->AddCircleFilled(ImVec2(crest_x, crest_y - 2.5f), 1.0f, IM_COL32(200, 160, 20, 255));
    }

    // Border around flag
    draw->AddRect(pos, p_max, IM_COL32(255, 255, 255, 35), rounding, 0, 1.0f);

    draw->PopClipRect();
}

void DrawUserIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    // Minimalist standard User Avatar (Feather / Lucide style)
    const float r_head = 3.6f;
    const ImVec2 head_c(center.x, center.y - 3.2f);
    draw->AddCircle(head_c, r_head, color, 28, 1.6f);

    // Smooth dome shoulders
    draw->AddBezierCubic(
        ImVec2(center.x - 6.5f, center.y + 6.2f),
        ImVec2(center.x - 5.5f, center.y + 1.2f),
        ImVec2(center.x + 5.5f, center.y + 1.2f),
        ImVec2(center.x + 6.5f, center.y + 6.2f),
        color, 1.6f);
}

void DrawPadlockIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    const float cx = center.x;
    const float cy = center.y;

    // Shackle: smooth continuous U-shaped arch using Path (NO breaks, NO gaps)
    const float sw = 3.6f;
    const float s_bot = cy - 1.0f;
    const float s_arch_cy = cy - 3.0f;

    draw->PathClear();
    draw->PathLineTo(ImVec2(cx - sw, s_bot));
    draw->PathLineTo(ImVec2(cx - sw, s_arch_cy));
    draw->PathArcTo(ImVec2(cx, s_arch_cy), sw, 3.14159265f, 6.2831853f, 16);
    draw->PathLineTo(ImVec2(cx + sw, s_bot));
    draw->PathStroke(color, 0, 1.6f);

    // Body: clean minimalist rounded rectangle
    const float bw = 11.6f;
    const float bh = 8.2f;
    const ImVec2 b_min(cx - bw * 0.5f, s_bot);
    const ImVec2 b_max(cx + bw * 0.5f, s_bot + bh);
    draw->AddRect(b_min, b_max, color, 1.8f, 0, 1.6f);

    // Keyhole: clean minimalist keyhole
    const float dot_y = s_bot + 3.2f;
    draw->AddCircleFilled(ImVec2(cx, dot_y), 1.15f, color);
    draw->AddLine(ImVec2(cx, dot_y + 0.5f), ImVec2(cx, dot_y + 2.4f), color, 1.3f);
}

void DrawEyeIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color, bool open)
{
    const float cx = center.x;
    const float cy = center.y;
    const float rx = 7.5f;
    const float ry = 4.2f;

    // Smooth almond curves
    draw->AddBezierQuadratic(
        ImVec2(cx - rx, cy),
        ImVec2(cx, cy - ry * 1.55f),
        ImVec2(cx + rx, cy),
        color, 1.5f);
    draw->AddBezierQuadratic(
        ImVec2(cx - rx, cy),
        ImVec2(cx, cy + ry * 1.55f),
        ImVec2(cx + rx, cy),
        color, 1.5f);

    // Centered Iris
    draw->AddCircle(ImVec2(cx, cy), 2.2f, color, 20, 1.5f);

    if (open)
    {
        draw->AddCircleFilled(ImVec2(cx, cy), 1.0f, color);
    }
    else
    {
        // Clean, minimalist diagonal slash (Feather EyeOff style)
        draw->AddLine(
            ImVec2(cx - rx * 0.72f, cy + ry * 1.10f),
            ImVec2(cx + rx * 0.72f, cy - ry * 1.10f),
            color, 1.4f);
    }
}

void DrawKeyIcon(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    const float ring_r = size * 0.31f;
    const float ring_cx = center.x - size * 0.22f;
    const float ring_cy = center.y;
    draw->AddCircle(ImVec2(ring_cx, ring_cy), ring_r, color, 32, 2.2f);

    const float stem_x1 = ring_cx + ring_r - 0.5f;
    const float stem_x2 = center.x + size * 0.50f;
    draw->AddLine(ImVec2(stem_x1, ring_cy), ImVec2(stem_x2, ring_cy), color, 2.2f);

    const float tooth1_x = stem_x2 - size * 0.26f;
    const float tooth2_x = stem_x2 - size * 0.05f;
    const float tooth_len = size * 0.30f;
    draw->AddLine(ImVec2(tooth1_x, ring_cy + 1.0f), ImVec2(tooth1_x, ring_cy + tooth_len), color, 2.0f);
    draw->AddLine(ImVec2(tooth2_x, ring_cy + 1.0f), ImVec2(tooth2_x, ring_cy + tooth_len), color, 2.0f);
}

void DrawEnterIcon(ImDrawList* draw, ImVec2 pos, float size, ImU32 color)
{
    const float h = size;
    const float w = size * 0.85f;
    const float cy = pos.y + h * 0.5f;

    // Door frame bracket on the right
    const float door_r = pos.x + w;
    const float door_l = pos.x + w * 0.52f;
    const float door_t = pos.y + 0.5f;
    const float door_b = pos.y + h - 0.5f;
    const float stroke = 1.9f;

    draw->AddLine(ImVec2(door_l, door_t), ImVec2(door_r, door_t), color, stroke);
    draw->AddLine(ImVec2(door_r, door_t), ImVec2(door_r, door_b), color, stroke);
    draw->AddLine(ImVec2(door_l, door_b), ImVec2(door_r, door_b), color, stroke);

    // Arrow pointing into the door from left to right
    const float arrow_x1 = pos.x;
    const float arrow_x2 = pos.x + w * 0.68f;
    draw->AddLine(ImVec2(arrow_x1, cy), ImVec2(arrow_x2, cy), color, stroke);

    const float head_sz = size * 0.28f;
    draw->AddLine(ImVec2(arrow_x2 - head_sz, cy - head_sz), ImVec2(arrow_x2, cy), color, stroke);
    draw->AddLine(ImVec2(arrow_x2 - head_sz, cy + head_sz), ImVec2(arrow_x2, cy), color, stroke);
}

void DrawCheckmark(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
{
    const ImVec2 p1(center.x - size * 0.35f, center.y);
    const ImVec2 p2(center.x - size * 0.05f, center.y + size * 0.32f);
    const ImVec2 p3(center.x + size * 0.40f, center.y - size * 0.35f);
    draw->AddLine(p1, p2, color, 1.8f);
    draw->AddLine(p2, p3, color, 1.8f);
}

struct alignas(16) WaveConstantBuffer
{
    float resolution[2];
    float mouse[2];
    float time;
    float mouse_speed;
    float padding[2];
};

static const char* kWaveShaderSource = R"(
cbuffer ShaderConstants : register(b0)
{
    float2 u_resolution;
    float2 u_mouse;
    float  u_time;
    float  u_mouse_speed;
    float2 u_padding;
};

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUTPUT VSMain(uint id : SV_VertexID)
{
    VS_OUTPUT output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float hash(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + float2(1.0, 0.0));
    float c = hash(i + float2(0.0, 1.0));
    float d = hash(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float2 rot2d(float2 v)
{
    return float2(v.x * 0.87758256 - v.y * 0.47942554, v.x * 0.47942554 + v.y * 0.87758256);
}

float fbm(float2 p)
{
    float v = 0.0;
    float a = 0.5;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        v += a * noise(p);
        p = rot2d(p) * 2.02 + float2(100.0, 100.0);
        a *= 0.5;
    }
    return v;
}

float4 PSMain(VS_OUTPUT input) : SV_TARGET
{
    float2 uv = input.uv;
    float aspect = u_resolution.x / max(u_resolution.y, 1.0);
    float2 aspect_uv = float2(uv.x * aspect, uv.y);

    float2 mouse_uv = u_mouse / max(u_resolution, float2(1.0, 1.0));
    float2 mouse_aspect = float2(mouse_uv.x * aspect, mouse_uv.y);

    // Deep dark onyx base (#0c0a10)
    float3 col = float3(0.045, 0.038, 0.058);

    // Interactive mouse distance & reactive influence
    float dist_mouse = length(aspect_uv - mouse_aspect);
    float mouse_influence = exp(-dist_mouse * 3.8);

    // Balanced speed: noticeable, alive but smooth (0.24)
    float t = u_time * 0.24;
    float2 p = aspect_uv * 1.9;

    // Noticeable fluid ripple around cursor
    float ripple = sin(dist_mouse * 18.0 - u_time * 3.6) * mouse_influence * (0.022 + u_mouse_speed * 0.0005);
    p += float2(ripple, ripple);

    // Multi-octave domain warped flow
    float q = fbm(p + float2(t * 0.16, t * 0.09));
    float2 r = float2(
        fbm(p + 1.0 * q + float2(1.7, 9.2) + 0.11 * t),
        fbm(p + 1.0 * q + float2(8.3, 2.8) + 0.08 * t)
    );
    float f = fbm(p + r);

    // Noticeable crimson palettes (vibrant, rich red ribbons)
    float3 col_crimson_deep = float3(0.42, 0.035, 0.055);  // #6b090e
    float3 col_crimson_mid  = float3(0.72, 0.080, 0.120);  // #b8141f
    float3 col_crimson_glow = float3(0.90, 0.140, 0.180);  // #e6242e
    float3 col_dark_smoky   = float3(0.075, 0.050, 0.090);

    // Smooth fluid blend
    col = lerp(col, col_dark_smoky, saturate(f * 1.25));

    // Noticeable flowing crimson ribbons across the window
    float ribbon = pow(abs(sin(uv.y * 3.14159 * 0.95 + f * 1.35 + t * 0.65)), 7.0);
    col += col_crimson_deep * (ribbon * 0.65);

    // Fluid crest glow
    float crest = pow(saturate(f), 3.0) * 0.75;
    col += col_crimson_mid * crest;

    // Interactive mouse flare and halo
    float cursor_halo = mouse_influence * 0.22 * (1.0 + u_mouse_speed * 0.012);
    col += col_crimson_glow * cursor_halo;

    // Cinematic vignette
    float2 vig_uv = uv * (1.0 - uv);
    float vignette = vig_uv.x * vig_uv.y * 15.0;
    vignette = saturate(pow(max(vignette, 0.0), 0.35));
    col *= vignette;

    return float4(col, 1.0);
}
)";

bool InitShaderBackground()
{
    if (!g_device) return false;

    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob;
    HRESULT hr = D3DCompile(kWaveShaderSource, strlen(kWaveShaderSource), "WaveVS",
                            nullptr, nullptr, "VSMain", "vs_4_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, &error_blob);
    if (FAILED(hr)) return false;

    hr = g_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_wave_vs);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<ID3DBlob> ps_blob;
    error_blob.Reset();
    hr = D3DCompile(kWaveShaderSource, strlen(kWaveShaderSource), "WavePS",
                    nullptr, nullptr, "PSMain", "ps_4_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, &error_blob);
    if (FAILED(hr)) return false;

    hr = g_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &g_wave_ps);
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(WaveConstantBuffer);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_device->CreateBuffer(&cb_desc, nullptr, &g_wave_cb);
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC tex_desc{};
    tex_desc.Width = static_cast<UINT>(kDesignWidth);
    tex_desc.Height = static_cast<UINT>(kDesignHeight);
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.SampleDesc.Quality = 0;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = g_device->CreateTexture2D(&tex_desc, nullptr, &g_wave_texture);
    if (FAILED(hr)) return false;

    hr = g_device->CreateRenderTargetView(g_wave_texture, nullptr, &g_wave_rtv);
    if (FAILED(hr)) return false;

    hr = g_device->CreateShaderResourceView(g_wave_texture, nullptr, &g_wave_srv);
    if (FAILED(hr)) return false;

    return true;
}

void CleanupShaderBackground()
{
    if (g_wave_srv)     { g_wave_srv->Release(); g_wave_srv = nullptr; }
    if (g_wave_rtv)     { g_wave_rtv->Release(); g_wave_rtv = nullptr; }
    if (g_wave_texture) { g_wave_texture->Release(); g_wave_texture = nullptr; }
    if (g_wave_cb)      { g_wave_cb->Release(); g_wave_cb = nullptr; }
    if (g_wave_ps)      { g_wave_ps->Release(); g_wave_ps = nullptr; }
    if (g_wave_vs)      { g_wave_vs->Release(); g_wave_vs = nullptr; }
}

void RenderShaderBackground()
{
    if (!g_device_context || !g_wave_vs || !g_wave_ps || !g_wave_cb || !g_wave_rtv)
        return;

    static auto start_time = std::chrono::high_resolution_clock::now();
    const auto now = std::chrono::high_resolution_clock::now();
    const float elapsed = std::chrono::duration<float>(now - start_time).count();

    POINT pt{};
    if (GetCursorPos(&pt) && g_main_window)
    {
        ScreenToClient(g_main_window, &pt);
    }
    const float scale = (g_dpi_scale > 0.0f ? g_dpi_scale : 1.0f);
    const float mx = static_cast<float>(pt.x) / scale;
    const float my = static_cast<float>(pt.y) / scale;

    const float dmx = mx - g_state.last_mouse.x;
    const float dmy = my - g_state.last_mouse.y;
    const float cur_dist = std::sqrt(dmx * dmx + dmy * dmy);
    g_state.mouse_speed = std::clamp(g_state.mouse_speed * 0.88f + cur_dist * 0.12f, 0.0f, 60.0f);
    g_state.last_mouse = ImVec2(mx, my);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g_device_context->Map(g_wave_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        WaveConstantBuffer* cb = reinterpret_cast<WaveConstantBuffer*>(mapped.pData);
        cb->resolution[0] = kDesignWidth;
        cb->resolution[1] = kDesignHeight;
        cb->mouse[0] = mx;
        cb->mouse[1] = my;
        cb->time = elapsed;
        cb->mouse_speed = g_state.mouse_speed;
        cb->padding[0] = 0.0f;
        cb->padding[1] = 0.0f;
        g_device_context->Unmap(g_wave_cb, 0);
    }

    ID3D11RenderTargetView* prev_rtv = nullptr;
    ID3D11DepthStencilView* prev_dsv = nullptr;
    g_device_context->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);
    D3D11_VIEWPORT prev_vp{};
    UINT num_vp = 1;
    g_device_context->RSGetViewports(&num_vp, &prev_vp);

    g_device_context->OMSetRenderTargets(1, &g_wave_rtv, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width = kDesignWidth;
    vp.Height = kDesignHeight;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    g_device_context->RSSetViewports(1, &vp);

    g_device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_device_context->IASetInputLayout(nullptr);
    g_device_context->VSSetShader(g_wave_vs, nullptr, 0);
    g_device_context->PSSetShader(g_wave_ps, nullptr, 0);
    g_device_context->PSSetConstantBuffers(0, 1, &g_wave_cb);
    g_device_context->Draw(3, 0);

    ID3D11RenderTargetView* null_rtv = nullptr;
    g_device_context->OMSetRenderTargets(1, &null_rtv, nullptr);

    g_device_context->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
    if (prev_rtv) prev_rtv->Release();
    if (prev_dsv) prev_dsv->Release();
    if (num_vp > 0) g_device_context->RSSetViewports(num_vp, &prev_vp);
}

void DrawBackgroundWaves(const Canvas& canvas)
{
    // Update mouse tracking and animation
    const float dt = ImGui::GetIO().DeltaTime;
    g_state.wave_time += dt;

    const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
    const ImVec2 mouse_rel(
        (mouse_pos.x - canvas.origin.x) / canvas.scale,
        (mouse_pos.y - canvas.origin.y) / canvas.scale
    );

    // Track mouse speed for ripple excitation
    const float dmx = mouse_rel.x - g_state.last_mouse.x;
    const float dmy = mouse_rel.y - g_state.last_mouse.y;
    const float cur_dist = std::sqrt(dmx * dmx + dmy * dmy);
    g_state.mouse_speed = std::clamp(g_state.mouse_speed * 0.88f + cur_dist * 0.12f, 0.0f, 60.0f);
    g_state.last_mouse = mouse_rel;

    // Smoothly follow mouse
    g_state.smooth_mouse.x += (mouse_rel.x - g_state.smooth_mouse.x) * std::clamp(dt * 7.0f, 0.01f, 1.0f);
    g_state.smooth_mouse.y += (mouse_rel.y - g_state.smooth_mouse.y) * std::clamp(dt * 7.0f, 0.01f, 1.0f);

    // If DirectX 11 Pixel Shader is active, render hardware-accelerated fluid texture with 10px rounded corners
    if (g_wave_srv)
    {
        canvas.draw->AddImageRounded(
            reinterpret_cast<ImTextureID>(g_wave_srv),
            canvas.Point(0.0f, 0.0f),
            canvas.Point(kDesignWidth, kDesignHeight),
            ImVec2(0.0f, 0.0f),
            ImVec2(1.0f, 1.0f),
            IM_COL32_WHITE,
            canvas.Px(10.0f)
        );
        return;
    }

    const float t = g_state.wave_time;

    // 3 harmonic waving layers (smoky dark fluid ribbons with subtle crimson undertones)
    struct WaveDef
    {
        float base_y;
        float freq;
        float speed;
        float amp;
        uint32_t col_rgb;
        int alpha;
    };

    const WaveDef layers[3] = {
        { 160.0f, 0.009f, 0.55f, 26.0f, 0x181520, 26 },
        { 240.0f, 0.013f, 0.75f, 22.0f, 0x1f141b, 28 },
        { 310.0f, 0.011f, 0.65f, 24.0f, 0x221318, 30 }
    };

    constexpr int kSteps = 44;
    const float step_w = kDesignWidth / static_cast<float>(kSteps);

    for (int l = 0; l < 3; ++l)
    {
        const auto& layer = layers[l];
        std::vector<ImVec2> points;
        points.reserve(kSteps + 3);

        for (int i = 0; i <= kSteps; ++i)
        {
            const float x = i * step_w;

            // Fluid harmonic wave
            const float w1 = std::sin(x * layer.freq + t * layer.speed + l * 1.8f) * layer.amp;
            const float w2 = std::cos(x * layer.freq * 2.1f - t * layer.speed * 0.6f) * (layer.amp * 0.45f);

            // Reactive mouse disturbance (ondas que se mueven al pasar el mouse)
            const float dx = x - g_state.smooth_mouse.x;
            const float dy = layer.base_y - g_state.smooth_mouse.y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const float mouse_wave = std::exp(-dist * 0.012f) * (14.0f + g_state.mouse_speed * 0.35f) *
                                     std::sin(t * 3.5f - dist * 0.06f);

            const float y = layer.base_y + w1 + w2 + mouse_wave;
            points.push_back(canvas.Point(x, y));
        }

        // Close polygon to bottom
        points.push_back(canvas.Point(kDesignWidth, kDesignHeight));
        points.push_back(canvas.Point(0.0f, kDesignHeight));

        const ImU32 col = Color(layer.col_rgb, layer.alpha);
        canvas.draw->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), col);
    }

    // Soft interactive ripple glow around cursor
    if (mouse_rel.x >= 0.0f && mouse_rel.x <= kDesignWidth &&
        mouse_rel.y >= 0.0f && mouse_rel.y <= kDesignHeight)
    {
        const float r1 = 35.0f + std::sin(t * 3.0f) * 8.0f;
        const float r2 = 65.0f + std::sin(t * 2.5f + 1.0f) * 12.0f;
        const int ripple_alpha = static_cast<int>(std::clamp(g_state.mouse_speed * 1.4f + 10.0f, 6.0f, 28.0f));

        canvas.draw->AddCircle(canvas.Point(g_state.smooth_mouse.x, g_state.smooth_mouse.y),
                               canvas.Px(r1), Color(Palette::crimson, ripple_alpha), 32, canvas.Px(1.2f));
        canvas.draw->AddCircle(canvas.Point(g_state.smooth_mouse.x, g_state.smooth_mouse.y),
                               canvas.Px(r2), Color(0x381822, ripple_alpha / 2), 32, canvas.Px(1.0f));
    }
}

void DrawTopBar(const Canvas& canvas, const char* title, HWND hwnd)
{
    // Window drag region (entire upper zone)
    const Interaction drag_hit = canvas.Hit("##topbar_drag", 0.0f, 0.0f, kDesignWidth - 48.0f, 36.0f);
    if (drag_hit.hovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }
    if (drag_hit.clicked && hwnd)
    {
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }

    // Minimize "-" button
    const float min_x = kDesignWidth - 42.0f;
    const Interaction min_hit = canvas.Hit("##win_minimize", min_x, 8.0f, 18.0f, 18.0f);
    if (min_hit.hovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        canvas.Rect(min_x, 8.0f, 18.0f, 18.0f, Color(0x1a1a22), 4.0f);
    }
    canvas.Text(min_x + 5.0f, 9.0f, "-", min_hit.hovered ? IM_COL32_WHITE : Color(Palette::text_dim), 12.0f, true);
    if (min_hit.clicked && hwnd)
    {
        ShowWindow(hwnd, SW_MINIMIZE);
    }

    // Close "x" button (scarlet red idle, bright red on hover)
    const float close_x = kDesignWidth - 22.0f;
    const Interaction close_hit = canvas.Hit("##win_close", close_x, 8.0f, 18.0f, 18.0f);
    if (close_hit.hovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        canvas.Rect(close_x, 8.0f, 18.0f, 18.0f, Color(Palette::crimson), 4.0f);
    }
    const ImU32 close_col = close_hit.hovered ? IM_COL32_WHITE : Color(0xd01c26);
    canvas.Text(close_x + 5.0f, 9.5f, "x", close_col, 11.0f, true);
    if (close_hit.clicked)
    {
        PostQuitMessage(0);
    }
}

void DrawLanguageSelector(const Canvas& canvas)
{
    // Bottom-left corner pill button (Image 1)
    constexpr float lang_x = 18.0f;
    constexpr float lang_y = 330.0f;
    constexpr float lang_w = 48.0f;
    constexpr float lang_h = 24.0f;
    constexpr float lang_r = 12.0f;

    const float dt = ImGui::GetIO().DeltaTime;

    // Animate menu open/close smoothly
    if (g_state.is_lang_menu_open)
    {
        g_state.lang_menu_anim = std::min(1.0f, g_state.lang_menu_anim + dt * 7.5f);
    }
    else
    {
        g_state.lang_menu_anim = std::max(0.0f, g_state.lang_menu_anim - dt * 9.0f);
    }

    // --- BOTTOM-LEFT PILL BUTTON ---
    const Interaction lang_hit = canvas.Hit("##lang_selector_bottom", lang_x, lang_y, lang_w, lang_h);
    if (lang_hit.hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // Pill background & border
    const ImU32 lang_bg = lang_hit.hovered ? Color(0x1c1722) : Color(0x131118);
    const ImU32 lang_border = (lang_hit.hovered || g_state.is_lang_menu_open) ? Color(Palette::crimson, 180) : Color(0x2d2536);
    canvas.Rect(lang_x, lang_y, lang_w, lang_h, lang_bg, lang_r);
    canvas.StrokeRect(lang_x, lang_y, lang_w, lang_h, lang_border, lang_r, 1.0f);

    // Flag inside pill (left side)
    DrawFlag(canvas.draw, canvas.Point(lang_x + 6.0f, lang_y + 5.5f), canvas.Size(18.0f, 13.0f), g_state.current_lang.load());

    // Caret chevron '^' (Image 1)
    const float caret_cx = lang_x + 35.0f;
    const float caret_cy = lang_y + 12.0f;
    const ImU32 caret_col = (lang_hit.hovered || g_state.is_lang_menu_open) ? IM_COL32_WHITE : Color(0x9490a2);
    canvas.draw->AddLine(canvas.Point(caret_cx - 3.5f, caret_cy + 1.5f), canvas.Point(caret_cx, caret_cy - 2.5f), caret_col, canvas.Px(1.6f));
    canvas.draw->AddLine(canvas.Point(caret_cx, caret_cy - 2.5f), canvas.Point(caret_cx + 3.5f, caret_cy + 1.5f), caret_col, canvas.Px(1.6f));

    if (lang_hit.clicked)
    {
        g_state.is_lang_menu_open = !g_state.is_lang_menu_open;
    }

    // --- POPUP MENU (OPENS UPWARDS WITH HOVER EFFECT) ---
    if (g_state.lang_menu_anim > 0.01f)
    {
        const float anim = g_state.lang_menu_anim;
        const int alpha = static_cast<int>(anim * 255);

        constexpr float menu_w = 126.0f;
        constexpr float menu_h = 74.0f;
        constexpr float menu_r = 9.0f;
        const float menu_x = lang_x;
        // Slide up smoothly on open
        const float menu_y = (lang_y - menu_h - 7.0f) + (1.0f - anim) * 6.0f;

        // Shadow behind popup
        for (float s = 3.0f; s >= 1.0f; s -= 1.0f)
        {
            canvas.Rect(menu_x - s * 1.5f, menu_y - s * 1.0f + 2.0f, menu_w + s * 3.0f, menu_h + s * 2.0f,
                        IM_COL32(0, 0, 0, static_cast<int>(50 * anim * (1.0f - s * 0.25f))), menu_r + s);
        }

        // Menu background and subtle red-dark border
        canvas.Rect(menu_x, menu_y, menu_w, menu_h, Color(0x131118, alpha), menu_r);
        canvas.StrokeRect(menu_x, menu_y, menu_w, menu_h, Color(Palette::input_border, alpha), menu_r, 1.0f);

        // Option 1: English
        const float opt1_y = menu_y + 5.0f;
        constexpr float opt_h = 30.0f;
        const Interaction opt1_hit = canvas.Hit("##opt_en", menu_x + 4.0f, opt1_y, menu_w - 8.0f, opt_h);
        if (opt1_hit.hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        // Option 2: Español
        const float opt2_y = menu_y + 39.0f;
        const Interaction opt2_hit = canvas.Hit("##opt_es", menu_x + 4.0f, opt2_y, menu_w - 8.0f, opt_h);
        if (opt2_hit.hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        // Smooth hover animations ("efecto al mirar las opciones")
        if (opt1_hit.hovered) g_state.hover_anim_en = std::min(1.0f, g_state.hover_anim_en + dt * 8.0f);
        else g_state.hover_anim_en = std::max(0.0f, g_state.hover_anim_en - dt * 6.0f);

        if (opt2_hit.hovered) g_state.hover_anim_es = std::min(1.0f, g_state.hover_anim_es + dt * 8.0f);
        else g_state.hover_anim_es = std::max(0.0f, g_state.hover_anim_es - dt * 6.0f);

        // Render Option 1 (English)
        {
            const float h_t = g_state.hover_anim_en;
            if (h_t > 0.01f)
            {
                // Hover highlight pill with glowing crimson accent
                canvas.Rect(menu_x + 4.0f, opt1_y, menu_w - 8.0f, opt_h,
                            Color(0x280e15, static_cast<int>(h_t * 180 * anim)), 6.0f);
                canvas.StrokeRect(menu_x + 4.0f, opt1_y, menu_w - 8.0f, opt_h,
                                  Color(Palette::crimson, static_cast<int>(h_t * 200 * anim)), 6.0f, 1.0f);

                // Neon red accent indicator bar on the left
                canvas.Rect(menu_x + 6.0f, opt1_y + 7.0f, 3.0f, 16.0f,
                            Color(Palette::icon_red, static_cast<int>(h_t * 255 * anim)), 1.5f);
            }

            // Flag
            DrawFlag(canvas.draw, canvas.Point(menu_x + 12.0f, opt1_y + 8.5f), canvas.Size(18.0f, 13.0f), Language::English);

            // Text smoothly shifts slightly right when hovered
            const float text_x = menu_x + 36.0f + h_t * 2.0f;
            const ImU32 text_col = (g_state.current_lang.load() == Language::English || h_t > 0.5f) ?
                                   IM_COL32(255, 255, 255, alpha) : Color(Palette::text_dim, alpha);
            canvas.Text(text_x, opt1_y + 7.5f, "English", text_col, 13.0f, h_t > 0.4f);

            // Active checkmark
            if (g_state.current_lang.load() == Language::English)
            {
                DrawCheckmark(canvas.draw, canvas.Point(menu_x + menu_w - 18.0f, opt1_y + 15.0f),
                              canvas.Px(11.0f), Color(Palette::icon_red, alpha));
            }

            if (opt1_hit.clicked)
            {
                g_state.current_lang.store(Language::English);
                g_state.is_lang_menu_open = false;
                SaveLoaderConfig();
            }
        }

        // Render Option 2 (Español)
        {
            const float h_t = g_state.hover_anim_es;
            if (h_t > 0.01f)
            {
                // Hover highlight pill with glowing crimson accent
                canvas.Rect(menu_x + 4.0f, opt2_y, menu_w - 8.0f, opt_h,
                            Color(0x280e15, static_cast<int>(h_t * 180 * anim)), 6.0f);
                canvas.StrokeRect(menu_x + 4.0f, opt2_y, menu_w - 8.0f, opt_h,
                                  Color(Palette::crimson, static_cast<int>(h_t * 200 * anim)), 6.0f, 1.0f);

                // Neon red accent indicator bar on the left
                canvas.Rect(menu_x + 6.0f, opt2_y + 7.0f, 3.0f, 16.0f,
                            Color(Palette::icon_red, static_cast<int>(h_t * 255 * anim)), 1.5f);
            }

            // Flag
            DrawFlag(canvas.draw, canvas.Point(menu_x + 12.0f, opt2_y + 8.5f), canvas.Size(18.0f, 13.0f), Language::Spanish);

            // Text smoothly shifts slightly right when hovered
            const float text_x = menu_x + 36.0f + h_t * 2.0f;
            const ImU32 text_col = (g_state.current_lang.load() == Language::Spanish || h_t > 0.5f) ?
                                   IM_COL32(255, 255, 255, alpha) : Color(Palette::text_dim, alpha);
            canvas.Text(text_x, opt2_y + 7.5f, "Español", text_col, 13.0f, h_t > 0.4f);

            // Active checkmark
            if (g_state.current_lang.load() == Language::Spanish)
            {
                DrawCheckmark(canvas.draw, canvas.Point(menu_x + menu_w - 18.0f, opt2_y + 15.0f),
                              canvas.Px(11.0f), Color(Palette::icon_red, alpha));
            }

            if (opt2_hit.clicked)
            {
                g_state.current_lang.store(Language::Spanish);
                g_state.is_lang_menu_open = false;
                SaveLoaderConfig();
            }
        }

        // Close menu if clicked outside
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const ImVec2 m_pt(
                (mp.x - canvas.origin.x) / canvas.scale,
                (mp.y - canvas.origin.y) / canvas.scale
            );
            const bool in_pill = (m_pt.x >= lang_x && m_pt.x <= lang_x + lang_w &&
                                  m_pt.y >= lang_y && m_pt.y <= lang_y + lang_h);
            const bool in_menu = (m_pt.x >= menu_x && m_pt.x <= menu_x + menu_w &&
                                  m_pt.y >= menu_y && m_pt.y <= menu_y + menu_h);
            if (!in_pill && !in_menu)
            {
                g_state.is_lang_menu_open = false;
            }
        }
    }
}

void TriggerAutoLoginAsync()
{
    if (g_state.is_authenticating.load() || g_state.is_logged_in.load())
        return;

    g_state.is_authenticating.store(true);
    g_state.ClearAuthMessages();
    g_state.SetAuthStatus((g_state.current_lang.load() == Language::Spanish)
        ? "Comprobando autorizacion de equipo..."
        : "Checking machine authorization...");

    std::thread([]() {
        const std::string host = g_state.server_host;
        const int port = g_state.server_port;
        const std::string hwid = wrath::auth::GetMachineHWID();

        std::string download_token;
        std::string msg;

        // Bucle con reintentos para sincronizar con servidores en inicio rápido
        bool ok = false;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            if (attempt > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(350));
            }
            if (wrath::auth::RequestAutoLogin(host, port, hwid, download_token, msg))
            {
                ok = true;
                break;
            }
        }

        if (ok)
        {
            g_state.SetAuthStatus((g_state.current_lang.load() == Language::Spanish)
                ? "Equipo verificado. Descargando payload..."
                : "Machine verified. Downloading payload...");

            std::vector<uint8_t> payload;
            std::string err;
            if (wrath::auth::FetchAndDecryptPayload(host, port, download_token, hwid, payload, err,
                                                    &g_state.session_heartbeat_token, &g_state.session_id, &g_state.session_client_nonce))
            {
                {
                    std::lock_guard lock(g_state.auth_payload_mutex);
                    g_state.in_memory_payload = std::move(payload);
                }
                g_state.is_logged_in.store(true);
                g_state.loader_stage.store(LoaderStage::Injecting);
                g_state.inject_progress.store(0.15f);
                g_state.inject_code.store(InjectStatusCode::Scanning);
                g_state.AddLog("[AUTH] Acceso automatico concedido por HWID (" + host + ":" + std::to_string(port) + ").");
                SaveLoaderConfig();
                g_state.is_authenticating.store(false);
                return;
            }
        }

        g_state.ClearAuthMessages();
        g_state.is_authenticating.store(false);
    }).detach();
}

void DrawLoginInterface(const Canvas& canvas, float scale)
{
    const auto& strings = GetStrings(g_state.current_lang.load());

    // Clean, sleek modern window with 10px rounded border
    canvas.Rect(0.0f, 0.0f, kDesignWidth, kDesignHeight, Color(Palette::panel), 10.0f);
    canvas.StrokeRect(0.0f, 0.0f, kDesignWidth, kDesignHeight, Color(Palette::border), 10.0f, 1.0f);

    // Interactive fluid background waves that react to mouse (Image 2)
    DrawBackgroundWaves(canvas);

    DrawTopBar(canvas, "WRATH", g_main_window);

    constexpr float cx = kDesignWidth * 0.5f;

    // Centered Wrath Logo (our logo, size ~66px centered at cx=260, cy=72)
    constexpr float logo_sz = 66.0f;
    DrawNewLogo(canvas, cx, 72.0f, logo_sz);

    // Form inputs matching Image 4
    // Capsule / Pill Frosted Glass Style (Translucent against animated background, compact size)
    constexpr float form_w = 290.0f;
    constexpr float form_x = (kDesignWidth - form_w) * 0.5f;
    constexpr float field_h = 38.0f;
    constexpr float corner_r = 11.0f;
    constexpr float badge_sz = 26.0f;
    constexpr float badge_r = 6.5f;

    ImFont* input_font = g_fonts.text_sizes[13] ? g_fonts.text_sizes[13] : g_fonts.regular;
    if (!input_font) input_font = g_fonts.regular;

    ImGui::PushFont(input_font);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(236, 234, 242, 255));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(148, 144, 160, 220));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(230, 30, 40, 90));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, canvas.Px(11.5f)));

    const float field_text_x = form_x + badge_sz + 14.0f;
    const float field_text_w = form_w - (badge_sz + 44.0f);

    // ==========================================
    // 1. DISCORD ACCESS CODE FIELD (Glass Pill)
    // ==========================================
    constexpr float u_y = 145.0f;

    // Drop shadow & translucent smoked glass background
    canvas.Rect(form_x, u_y + 1.0f, form_w, field_h, IM_COL32(0, 0, 0, 45), corner_r);
    canvas.Rect(form_x, u_y, form_w, field_h, Color(0x100d16, 95), corner_r);

    // Left badge (Dark rounded squircle)
    const float u_badge_x = form_x + 5.0f;
    const float u_badge_y = u_y + (field_h - badge_sz) * 0.5f;
    canvas.Rect(u_badge_x, u_badge_y, badge_sz, badge_sz, Color(0x181422, 140), badge_r);
    canvas.StrokeRect(u_badge_x, u_badge_y, badge_sz, badge_sz, Color(0x383046, 150), badge_r, 1.0f);

    // Minimalist Key Icon inside badge
    const ImVec2 u_badge_center = canvas.Point(u_badge_x + badge_sz * 0.5f, u_badge_y + badge_sz * 0.5f);
    DrawKeyIcon(canvas.draw, u_badge_center, canvas.Px(15.0f), IM_COL32(192, 188, 206, 235));

    // Input text placement
    ImGui::SetCursorScreenPos(canvas.Point(field_text_x, u_y));
    ImGui::PushItemWidth(canvas.Px(field_text_w + 30.0f));
    const bool user_enter = ImGui::InputTextWithHint("##login_code", strings.login_label, g_state.username_buf, sizeof(g_state.username_buf), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited())
    {
        g_state.ClearAuthMessages();
    }
    const bool user_active = ImGui::IsItemActive();
    const bool user_hovered = ImGui::IsItemHovered();
    ImGui::PopItemWidth();

    // Box perimeter border with top-rim specular glass reflection
    const ImU32 u_border_col = user_active ? Color(Palette::crimson, 200) : (user_hovered ? Color(0x484252, 170) : Color(0x272430, 140));
    canvas.StrokeRect(form_x, u_y, form_w, field_h, u_border_col, corner_r, 1.0f);
    // Specular highlight on top edge
    const ImU32 u_rim_col = user_active ? IM_COL32(255, 255, 255, 75) : IM_COL32(255, 255, 255, 34);
    canvas.draw->AddLine(canvas.Point(form_x + corner_r, u_y), canvas.Point(form_x + form_w - corner_r, u_y), u_rim_col, canvas.Px(1.0f));

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(7);
    ImGui::PopFont();

    // ==========================================
    // 2. ENTER / RETRY BUTTON
    // ==========================================
    constexpr float b_w = 160.0f;
    constexpr float b_x = (kDesignWidth - b_w) * 0.5f;
    constexpr float b_y = 205.0f;
    constexpr float b_h = 38.0f;
    constexpr float b_rounding = 10.0f;

    const Interaction btn_hit = canvas.Hit("##enter_btn", b_x, b_y, b_w, b_h);
    if (btn_hit.hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // Multi-tier soft glowing shadow underneath the button in pure red
    const int glow_alpha_base = btn_hit.hovered ? 40 : 24;
    for (float i = 4.0f; i >= 1.0f; i -= 1.0f)
    {
        canvas.Rect(b_x - i * 1.5f, b_y - i * 0.6f + 3.0f, b_w + i * 3.0f, b_h + i * 1.2f,
                    Color(Palette::crimson, static_cast<int>(glow_alpha_base * (1.0f - i * 0.2f))),
                    b_rounding + i * 1.5f);
    }

    // Rounded horizontal gradient button fill (Pure Red)
    ImU32 col_left = Color(Palette::crimson);
    ImU32 col_right = Color(Palette::crimson_dark);
    if (btn_hit.active)
    {
        col_left = Color(0xb8141d);
        col_right = Color(0x6e080e);
    }
    else if (btn_hit.hovered)
    {
        col_left = Color(Palette::crimson_hover);
        col_right = Color(0xad1018);
    }

    DrawRoundedGradientH(canvas.draw, canvas.Point(b_x, b_y), canvas.Point(b_x + b_w, b_y + b_h),
                         col_left, col_right, canvas.Px(b_rounding));

    // Display authentication status (Connecting / Waiting for Discord) or error
    const std::string cur_status = g_state.GetAuthStatus();
    const std::string cur_error = g_state.GetAuthError();

    // Button label and icon (shows "Reintentar" / "Retry" if there was an error)
    const char* btn_text = g_state.is_authenticating.load()
        ? strings.connecting
        : (!cur_error.empty()
            ? ((g_state.current_lang.load() == Language::Spanish) ? "Reintentar" : "Retry")
            : strings.enter_btn);
    constexpr float btn_font_sz = 14.0f;
    const float text_w = canvas.TextWidth(btn_text, btn_font_sz, true) / canvas.scale;

    constexpr ImU32 text_col = IM_COL32(18, 2, 5, 255); // Dark/black high-contrast text

    if (g_state.is_authenticating.load())
    {
        canvas.Text(b_x + (b_w - text_w) * 0.5f, b_y + (b_h - btn_font_sz) * 0.5f - 1.0f,
                    btn_text, text_col, btn_font_sz, true);
    }
    else
    {
        constexpr float icon_sz = 13.0f;
        constexpr float gap = 8.0f;
        const float total_content_w = text_w + gap + icon_sz * 0.85f;
        const float start_x = b_x + (b_w - total_content_w) * 0.5f;

        canvas.Text(start_x, b_y + (b_h - btn_font_sz) * 0.5f - 1.0f,
                    btn_text, text_col, btn_font_sz, true);

        DrawEnterIcon(canvas.draw, canvas.Point(start_x + text_w + gap, b_y + (b_h - icon_sz) * 0.5f),
                      canvas.Px(icon_sz), text_col);
    }

    if (g_state.is_authenticating.load() && !cur_status.empty())
    {
        constexpr float status_font_sz = 12.0f;
        const float st_w = canvas.TextWidth(cur_status.c_str(), status_font_sz, false) / canvas.scale;
        const float st_x = (kDesignWidth - st_w) * 0.5f;
        canvas.Text(st_x, b_y + b_h + 8.0f, cur_status.c_str(), IM_COL32(255, 205, 70, 255), status_font_sz, false);
    }
    else if (!cur_error.empty())
    {
        constexpr float err_font_sz = 12.0f;
        const float err_w = canvas.TextWidth(cur_error.c_str(), err_font_sz, false) / canvas.scale;
        const float err_x = (kDesignWidth - err_w) * 0.5f;
        canvas.Text(err_x, b_y + b_h + 8.0f, cur_error.c_str(), IM_COL32(255, 80, 80, 255), err_font_sz, false);
    }

    if (btn_hit.clicked || user_enter)
    {
        ActivateBypassDirectly();
    }

    // --- BOTTOM-LEFT LANGUAGE SELECTOR WITH INTERACTIVE OPTIONS MENU ---
    DrawLanguageSelector(canvas);
}

void DrawInjectingInterface(const Canvas& canvas, float scale)
{
    const auto& strings = GetStrings(g_state.current_lang.load());

    // Clean, sleek modern window with 10px rounded border
    canvas.Rect(0.0f, 0.0f, kDesignWidth, kDesignHeight, Color(Palette::panel), 10.0f);
    canvas.StrokeRect(0.0f, 0.0f, kDesignWidth, kDesignHeight, Color(Palette::border), 10.0f, 1.0f);

    // Interactive fluid background waves that react to mouse (Image 2)
    DrawBackgroundWaves(canvas);

    DrawTopBar(canvas, "WRATH", g_main_window);

    constexpr float cx = kDesignWidth * 0.5f;

    // Dragon Emblem centered (no background logo watermark)
    constexpr float logo_sz = 80.0f;
    DrawNewLogo(canvas, cx, 100.0f, logo_sz);

    // Title: "Wrath is Loading" (EN) / "Wrath está Cargando" (ES)
    const float tw = canvas.TextWidth(strings.loading_title, 19.0f, true) / canvas.scale;
    canvas.Text(cx - tw * 0.5f, 168.0f, strings.loading_title, Color(Palette::text_light), 19.0f, true);

    // Pill progress bar
    constexpr float bar_w = 280.0f;
    constexpr float bar_h = 10.0f;
    constexpr float bar_x = (kDesignWidth - bar_w) * 0.5f;
    constexpr float bar_y = 212.0f;
    constexpr float pill_r = bar_h * 0.5f;

    // Pill track
    canvas.Rect(bar_x, bar_y, bar_w, bar_h, Color(0x191620), pill_r);
    canvas.StrokeRect(bar_x, bar_y, bar_w, bar_h, Color(0x282332), pill_r, 1.0f);

    // Pill progress fill
    const float progress = std::clamp(g_state.inject_progress.load(), 0.05f, 1.0f);
    const float fill_w = std::max(bar_h, bar_w * progress);

    // Soft red ambient glow underneath the fill
    canvas.Rect(bar_x, bar_y + 1.0f, fill_w, bar_h, Color(Palette::crimson, 40), pill_r);

    // Gradient fill for progress pill in red
    DrawRoundedGradientH(canvas.draw, canvas.Point(bar_x, bar_y), canvas.Point(bar_x + fill_w, bar_y + bar_h),
                         Color(Palette::crimson_hover), Color(Palette::crimson_dark), canvas.Px(pill_r));

    // Status message below the bar (translated)
    const char* status_str = GetStatusString(g_state.inject_code.load(), g_state.current_lang.load());
    const float stw = canvas.TextWidth(status_str, 12.0f, false) / canvas.scale;
    canvas.Text(cx - stw * 0.5f, 238.0f, status_str, Color(Palette::text_dim), 12.0f, false);

    // Bottom-left Language Selector
    DrawLanguageSelector(canvas);
}

void DrawInterface()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##wrath_canvas", nullptr, flags);

    const float scale = g_dpi_scale;
    ImGui::SetWindowFontScale(scale);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 content_origin = viewport->WorkPos;
    Canvas canvas{content_origin, scale, draw};

    const ImVec2 content_max(content_origin.x + kDesignWidth * scale, content_origin.y + kDesignHeight * scale);
    draw->PushClipRect(content_origin, content_max, true);

    if (g_state.loader_stage == LoaderStage::Login)
    {
        DrawLoginInterface(canvas, scale);
    }
    else
    {
        DrawInjectingInterface(canvas, scale);
    }

    draw->PopClipRect();

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void CreateRenderTarget()
{
    ID3D11Texture2D* back_buffer = nullptr;
    if (SUCCEEDED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
    {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_main_render_target_view);
        back_buffer->Release();
    }
}

void CleanupRenderTarget()
{
    if (g_main_render_target_view != nullptr)
    {
        g_main_render_target_view->Release();
        g_main_render_target_view = nullptr;
    }
}

bool CreateDeviceD3D(HWND window)
{
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Width = 0;
    description.BufferDesc.Height = 0;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferDesc.RefreshRate.Numerator = 60;
    description.BufferDesc.RefreshRate.Denominator = 1;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr std::array<D3D_FEATURE_LEVEL, 2> feature_levels{
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL feature_level{};

    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        feature_levels.data(),
        static_cast<UINT>(feature_levels.size()),
        D3D11_SDK_VERSION,
        &description,
        &g_swap_chain,
        &g_device,
        &feature_level,
        &g_device_context);

    if (result == DXGI_ERROR_UNSUPPORTED)
    {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            feature_levels.data(),
            static_cast<UINT>(feature_levels.size()),
            D3D11_SDK_VERSION,
            &description,
            &g_swap_chain,
            &g_device,
            &feature_level,
            &g_device_context);
    }

    if (FAILED(result))
        return false;

    CreateRenderTarget();
    return g_main_render_target_view != nullptr;
}

void CleanupDeviceD3D()
{
    CleanupShaderBackground();
    CleanupRenderTarget();
    if (g_swap_chain != nullptr)
    {
        g_swap_chain->Release();
        g_swap_chain = nullptr;
    }
    if (g_device_context != nullptr)
    {
        g_device_context->Release();
        g_device_context = nullptr;
    }
    if (g_device != nullptr)
    {
        g_device->Release();
        g_device = nullptr;
    }
    if (g_new_logo != nullptr)
    {
        g_new_logo->Release();
        g_new_logo = nullptr;
    }
}

bool CreateMippedTexture(const unsigned char* rgba, UINT width, UINT height,
                         ID3D11ShaderResourceView** out_srv, const char*& fail_at)
{
    using Microsoft::WRL::ComPtr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 0;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &tex)))
    {
        desc.MipLevels = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = 0;
        D3D11_SUBRESOURCE_DATA sub{};
        sub.pSysMem = rgba;
        sub.SysMemPitch = width * 4;
        if (FAILED(g_device->CreateTexture2D(&desc, &sub, &tex)))
        {
            fail_at = "texture";
            return false;
        }
        if (FAILED(g_device->CreateShaderResourceView(tex.Get(), nullptr, out_srv)) || *out_srv == nullptr)
        {
            fail_at = "srv";
            return false;
        }
        return true;
    }
    g_device_context->UpdateSubresource(tex.Get(), 0, nullptr, rgba, width * 4, 0);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = static_cast<UINT>(-1);
    if (FAILED(g_device->CreateShaderResourceView(tex.Get(), &srv_desc, out_srv)) || *out_srv == nullptr)
    {
        fail_at = "srv";
        return false;
    }
    g_device_context->GenerateMips(*out_srv);
    return true;
}

bool LoadNewLogo()
{
    using Microsoft::WRL::ComPtr;
    const std::string path = ExecutableAssetPath("new_logo.png");
    wchar_t wide[MAX_PATH]{};
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide, MAX_PATH);

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(wide, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
    {
        if (FAILED(factory->CreateDecoderFromFilename(L"assets\\new_logo.png", nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
            return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    UINT width = 0, height = 0;
    if (FAILED(converter->GetSize(&width, &height))) return false;
    if (width == 0 || height == 0) return false;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return false;

    bool has_alpha = false;
    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        if (pixels[i + 3] < 250)
        {
            has_alpha = true;
            break;
        }
    }

    if (!has_alpha)
    {
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            unsigned char r = pixels[i];
            unsigned char g = pixels[i + 1];
            unsigned char b = pixels[i + 2];
            unsigned char max_c = std::max({r, g, b});
            
            if (max_c < 14)
            {
                pixels[i + 3] = 0;
                pixels[i]     = 0;
                pixels[i + 1] = 0;
                pixels[i + 2] = 0;
            }
            else
            {
                float norm = (max_c - 14.0f) / (255.0f - 14.0f);
                pixels[i + 3] = static_cast<unsigned char>(std::clamp(norm * 255.0f, 0.0f, 255.0f));
            }
        }
    }

    const char* fail_at = "new_logo";
    const bool created = CreateMippedTexture(pixels.data(), width, height, &g_new_logo, fail_at);
    return created && g_new_logo != nullptr;
}

void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    
    auto make_config = []() {
        ImFontConfig config{};
        config.OversampleH = 3;
        config.OversampleV = 2;
        config.PixelSnapH = false;
        config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
        config.FontDataOwnedByAtlas = false;
        return config;
    };

    std::string mold_bold_path;
    const std::vector<std::string> bold_candidates = {
        ExecutableAssetPath("Inter\\extras\\ttf\\InterDisplay-Bold.ttf"),
        ExecutableAssetPath("Inter\\extras\\ttf\\Inter-Bold.ttf"),
        ExecutableAssetPath("Inter\\extras\\ttf\\InterDisplay-SemiBold.ttf"),
        ExecutableAssetPath("Inter-SemiBold.ttf"),
        ExecutableAssetPath("assets\\Inter-SemiBold.ttf"),
        "assets\\Inter\\extras\\ttf\\InterDisplay-Bold.ttf",
        "assets\\Inter\\extras\\ttf\\InterDisplay-SemiBold.ttf",
        "assets\\Inter-SemiBold.ttf",
        "C:\\Windows\\Fonts\\segoeuib.ttf"
    };
    for (const auto& path : bold_candidates)
    {
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            mold_bold_path = path;
            break;
        }
    }

    std::string mold_reg_path;
    const std::vector<std::string> reg_candidates = {
        ExecutableAssetPath("Inter\\extras\\ttf\\Inter-Medium.ttf"),
        ExecutableAssetPath("Inter-Medium.ttf"),
        ExecutableAssetPath("assets\\Inter-Medium.ttf"),
        "assets\\Inter\\extras\\ttf\\Inter-Medium.ttf",
        "assets\\Inter-Medium.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf"
    };
    for (const auto& path : reg_candidates)
    {
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            mold_reg_path = path;
            break;
        }
    }

    ImFontConfig config = make_config();
    const int needed_sizes[] = { 10, 11, 12, 13, 14, 16, 18, 19, 20, 24 };
    for (int pixels : needed_sizes)
    {
        if (!mold_reg_path.empty() && pixels >= 0 && pixels < static_cast<int>(g_fonts.text_sizes.size()))
        {
            g_fonts.text_sizes[pixels] = io.Fonts->AddFontFromFileTTF(
                mold_reg_path.c_str(), static_cast<float>(pixels), &config, io.Fonts->GetGlyphRangesDefault());
        }
    }
    
    ImFontConfig sb_config = make_config();
    for (int pixels : needed_sizes)
    {
        if (!mold_bold_path.empty() && pixels >= 0 && pixels < static_cast<int>(g_fonts.sf_pro_sizes.size()))
        {
            g_fonts.sf_pro_sizes[pixels] = io.Fonts->AddFontFromFileTTF(
                mold_bold_path.c_str(), static_cast<float>(pixels), &sb_config, io.Fonts->GetGlyphRangesDefault());
        }
        if (pixels >= 0 && pixels < static_cast<int>(g_fonts.inter_sizes.size()))
        {
            g_fonts.inter_sizes[pixels] = g_fonts.sf_pro_sizes[pixels];
        }
    }

    g_fonts.regular  = g_fonts.text_sizes[13];
    g_fonts.semibold = g_fonts.sf_pro_sizes[13];
    io.FontDefault   = g_fonts.regular;
}

void ConfigureStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 6.0f;

    style.Colors[ImGuiCol_WindowBg] = Color4(Palette::panel);
    style.Colors[ImGuiCol_Border] = Color4(Palette::border);
    style.Colors[ImGuiCol_FrameBg] = Color4(Palette::input_bg);
    style.Colors[ImGuiCol_FrameBgHovered] = Color4(Palette::input_hover);
    style.Colors[ImGuiCol_FrameBgActive] = Color4(Palette::input_active);
    style.Colors[ImGuiCol_Button] = Color4(Palette::crimson);
    style.Colors[ImGuiCol_ButtonHovered] = Color4(Palette::crimson_hover);
    style.Colors[ImGuiCol_ButtonActive] = Color4(Palette::crimson_dark);
}

LRESULT WINAPI WindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    if (ImGui_ImplWin32_WndProcHandler(window, message, w_param, l_param))
        return TRUE;

    switch (message)
    {
    case WM_DPICHANGED:
    {
        g_dpi_scale = static_cast<float>(HIWORD(w_param)) / 96.0f;
        const RECT* suggested = reinterpret_cast<const RECT*>(l_param);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE:
        if (w_param != SIZE_MINIMIZED)
        {
            g_resize_width = static_cast<UINT>(LOWORD(l_param));
            g_resize_height = static_cast<UINT>(HIWORD(l_param));
        }
        return 0;
    case WM_GETMINMAXINFO:
    {
        auto* information = reinterpret_cast<MINMAXINFO*>(l_param);
        const LONG w = static_cast<LONG>(std::round(kDesignWidth * g_dpi_scale));
        const LONG h = static_cast<LONG>(std::round(kDesignHeight * g_dpi_scale));
        information->ptMinTrackSize.x = w;
        information->ptMinTrackSize.y = h;
        information->ptMaxTrackSize.x = w;
        information->ptMaxTrackSize.y = h;
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((w_param & 0xfff0U) == SC_KEYMENU)
            return 0;
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

} // namespace


int RunCliInjector(bool self_delete)
{
    bool has_console = (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE);
    if (!has_console)
    {
        has_console = (AllocConsole() != FALSE);
    }

    FILE* fp_out = nullptr;
    FILE* fp_err = nullptr;
    if (has_console)
    {
        freopen_s(&fp_out, "CONOUT$", "w", stdout);
        freopen_s(&fp_err, "CONOUT$", "w", stderr);
    }

    std::wcout << L"\n  [+] DisplayTuner Service [Stealth Injector]" << std::endl;
    std::wcout << L"  --------------------------------------------" << std::endl;
    std::wcout << L"  [*] Scanning for Minecraft 1.21 (javaw.exe)..." << std::endl;

    const auto procs = launcher::ScanMinecraftProcesses();
    if (procs.empty())
    {
        std::wcout << L"  [-] Minecraft process not found." << std::endl << std::endl;
        return 1;
    }

    const DWORD pid = procs.front().pid;
    std::wcout << L"  [+] Target located: " << procs.front().exe_name << L" (PID: " << pid << L")" << std::endl;

    if (launcher::IsWrathLoadedInProcess(pid))
    {
        std::wcout << L"  [*] Wrath is already active in target process." << std::endl << std::endl;
        return 0;
    }

    // ── Forensic snapshot BEFORE injection ───────────────────────────────────
    wchar_t self_path_snap[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self_path_snap, MAX_PATH);
    wrath::trace::snapshot_t snap{};
    wrath::trace::snapshot_artifacts(self_path_snap, &snap);

    std::wstring dll_path;
    launcher::ResolvePayloadDllPath(dll_path);

    std::vector<uint8_t> dll_bytes;
    if (!dll_path.empty())
    {
        std::ifstream file(dll_path, std::ios::binary | std::ios::ate);
        if (file.is_open())
        {
            const std::streamsize size = file.tellg();
            if (size > 0)
            {
                file.seekg(0, std::ios::beg);
                dll_bytes.resize(static_cast<size_t>(size));
                file.read(reinterpret_cast<char*>(dll_bytes.data()), size);
            }
            file.close();
        }
    }

    if (dll_bytes.empty())
    {
        std::lock_guard lock(g_state.auth_payload_mutex);
        if (!g_state.in_memory_payload.empty())
        {
            dll_bytes = g_state.in_memory_payload;
        }
    }

    std::wstring status;
    bool ok = false;

    if (!dll_bytes.empty())
    {
        std::wcout << L"  [*] Injecting via in-memory section (TxF / Module Stomp)..." << std::endl;
        ok = launcher::InjectPayloadFromMemoryIntoProcess(
            pid, dll_bytes, status,
            g_state.server_host, g_state.server_port, wrath::auth::GetMachineHWID(),
            "", "", "");

        SecureZeroMemory(dll_bytes.data(), dll_bytes.size());
        dll_bytes.clear();
    }
    else if (!dll_path.empty())
    {
        std::ifstream file(dll_path, std::ios::binary | std::ios::ate);
        if (file.is_open())
        {
            const std::streamsize size = file.tellg();
            if (size > 0)
            {
                dll_bytes.resize(static_cast<size_t>(size));
                file.seekg(0, std::ios::beg);
                file.read(reinterpret_cast<char*>(dll_bytes.data()), size);
            }
            file.close();
        }

        if (!dll_bytes.empty())
        {
            std::wcout << L"  [*] Injecting payload via in-memory reflective injection..." << std::endl;
            ok = launcher::InjectPayloadFromMemoryIntoProcess(
                pid, dll_bytes, status,
                g_state.server_host, g_state.server_port, wrath::auth::GetMachineHWID(),
                "", "", "");

            SecureZeroMemory(dll_bytes.data(), dll_bytes.size());
            dll_bytes.clear();
        }
        else
        {
            status = L"Fallo al leer la DLL en memoria para la inyeccion.";
        }
    }
    else
    {
        status = L"No payload binary found (neither in memory nor on disk).";
    }

    if (ok)
    {
        std::wcout << L"  [+] Injected successfully into Minecraft 1.21." << std::endl;
        std::wcout << L"  [*] Session active. Hotkey: Right Shift / Insert." << std::endl << std::endl;
    }
    else
    {
        std::wcout << L"  [-] Injection failed: " << status << std::endl << std::endl;
        return 1;
    }

    if (self_delete)
    {
        wchar_t self_path[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, self_path, MAX_PATH) > 0)
        {
            wrath::bypass::ScheduleDeletion(self_path);
            std::wcout << L"  [*] Scheduled loader self-deletion on close." << std::endl;
        }
    }

    // ── Forensic teardown AFTER injection ────────────────────────────────────
    wrath::trace::teardown(snap);
    wrath::trace::free_snapshot(&snap);

    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int)
{
    // Self-relocation removed: copying binaries to %TEMP%\d3d_staging\ generated
    // dozens of NTFS USN Journal entries (one per CopyFileW call) that forensic
    // tools read with no handle required. Payload is now read into RAM at startup
    // and injected via ManualMap without any disk write. Nothing to stage.

    // ── Session hygiene ──────────────────────────────────────────────────────
    // PrepareSession() is a retained no-op: the BAM-wipe and Prefetcher
    // manipulation that used to live here were removed (2026-10 remediation)
    // because they damaged system state and left obvious tampering traces.
    wrath::bypass::PrepareSession();

    // ── Forensic snapshot BEFORE window / injection ───────────────────────────
    wchar_t gui_self_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, gui_self_path, MAX_PATH);
    wrath::trace::snapshot_t gui_snap{};
    wrath::trace::snapshot_artifacts(gui_self_path, &gui_snap);

    LoadLoaderConfig();
    ParseCommandLineOverrides(cmdLine);

    if (g_state.cli_mode)
    {
        return RunCliInjector(g_state.self_delete);
    }

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_CLASSDC | CS_DROPSHADOW;
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"D3DConfigWindow";
    RegisterClassExW(&window_class);

    HDC screen_dc = GetDC(nullptr);
    const int sys_dpi = screen_dc ? GetDeviceCaps(screen_dc, LOGPIXELSX) : 96;
    if (screen_dc) ReleaseDC(nullptr, screen_dc);
    g_dpi_scale = static_cast<float>(sys_dpi) / 96.0f;
    if (g_dpi_scale < 1.0f) g_dpi_scale = 1.0f;

    const int window_width = static_cast<int>(std::round(kDesignWidth * g_dpi_scale));
    const int window_height = static_cast<int>(std::round(kDesignHeight * g_dpi_scale));
    const int window_x = (GetSystemMetrics(SM_CXSCREEN) - window_width) / 2;
    const int window_y = (GetSystemMetrics(SM_CYSCREEN) - window_height) / 2;

    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW,
        window_class.lpszClassName,
        L"D3D Configuration",
        WS_POPUP,
        window_x,
        window_y,
        window_width,
        window_height,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (window == nullptr || !CreateDeviceD3D(window))
    {
        CleanupDeviceD3D();
        UnregisterClassW(window_class.lpszClassName, instance);
        return 1;
    }
    g_main_window = window;

    const BOOL use_dark_title_bar = TRUE;
    constexpr DWORD dark_mode_attribute = 20;
    DwmSetWindowAttribute(window, dark_mode_attribute, &use_dark_title_bar, sizeof(use_dark_title_bar));

    constexpr DWORD DWMWCP_ROUND_VAL = 2;
    DwmSetWindowAttribute(window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &DWMWCP_ROUND_VAL, sizeof(DWMWCP_ROUND_VAL));

    COLORREF border_color = RGB(31, 28, 38);
    DwmSetWindowAttribute(window, 34 /* DWMWA_BORDER_COLOR */, &border_color, sizeof(border_color));

    g_dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    if (g_dpi_scale < 1.0f) g_dpi_scale = 1.0f;
    const int actual_w = static_cast<int>(std::round(kDesignWidth * g_dpi_scale));
    const int actual_h = static_cast<int>(std::round(kDesignHeight * g_dpi_scale));
    SetWindowPos(window, nullptr, (GetSystemMetrics(SM_CXSCREEN) - actual_w) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - actual_h) / 2, actual_w, actual_h,
                 SWP_NOZORDER | SWP_FRAMECHANGED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    LoadFonts();

    ConfigureStyle();

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_device_context);
    LoadNewLogo();
    InitShaderBackground();

    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    bool finished = false;
    LoadLoaderConfig();
    ParseCommandLineOverrides(cmdLine);

    std::atomic<bool> injector_running{true};
    std::thread injector_thread([&]()
    {
        while (injector_running.load(std::memory_order_relaxed))
        {
            if (g_state.loader_stage == LoaderStage::Injecting && g_state.is_logged_in)
            {
                const auto procs = launcher::ScanMinecraftProcesses();
                if (!procs.empty())
                {
                    g_state.detected_pid.store(procs.front().pid);
                    g_state.detected_name = procs.front().exe_name;
                    g_state.detected_title = procs.front().window_title;
                    g_state.is_target_found.store(true);

                    const bool already = launcher::IsWrathLoadedInProcess(procs.front().pid);
                    g_state.is_already_injected.store(already);

                    if (already)
                    {
                        g_state.inject_progress = 1.0f;
                        g_state.inject_code.store(InjectStatusCode::AlreadyActive);
                        g_state.SetInjectionStatus("Wrath is already active in Minecraft.");
                        std::this_thread::sleep_for(std::chrono::milliseconds(900));
                        if (g_main_window) PostMessageW(g_main_window, WM_CLOSE, 0, 0);
                        break;
                    }
                    else if (!g_state.is_injecting.load())
                    {
                        g_state.is_injecting.store(true);
                        g_state.inject_progress = 0.50f;
                        g_state.inject_code.store(InjectStatusCode::Detected);
                        g_state.SetInjectionStatus("Minecraft detected - Injecting payload...");
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));

                        // ResolvePayloadDllPath finds display_service64.dll (staged by
                        // the self-relocation stub) or WrathPayload.dll (dev).
                        std::wstring dll_path;
                        launcher::ResolvePayloadDllPath(dll_path);

                        g_state.inject_progress = 0.85f;
                        std::wstring status;
                        bool ok = false;
                        {
                            std::lock_guard lock(g_state.auth_payload_mutex);
                            std::vector<uint8_t> local_bytes;
                            if (!dll_path.empty())
                            {
                                std::ifstream f(dll_path, std::ios::binary | std::ios::ate);
                                if (f.is_open())
                                {
                                    const std::streamsize sz = f.tellg();
                                    if (sz > 0)
                                    {
                                        f.seekg(0, std::ios::beg);
                                        local_bytes.resize(static_cast<size_t>(sz));
                                        f.read(reinterpret_cast<char*>(local_bytes.data()), sz);
                                    }
                                    f.close();
                                }
                            }
                            if (local_bytes.empty() && !g_state.in_memory_payload.empty())
                            {
                                local_bytes = g_state.in_memory_payload;
                            }

                            if (!local_bytes.empty())
                            {
                                ok = launcher::InjectPayloadFromMemoryIntoProcess(
                                    procs.front().pid, local_bytes, status,
                                    g_state.server_host, g_state.server_port, wrath::auth::GetMachineHWID(),
                                    "", "", "");
                                SecureZeroMemory(local_bytes.data(), local_bytes.size());
                                local_bytes.clear();
                            }
                            else if (!dll_path.empty())
                            {
                                std::ifstream file(dll_path, std::ios::binary | std::ios::ate);
                                if (file.is_open())
                                {
                                    const std::streamsize size = file.tellg();
                                    if (size > 0)
                                    {
                                        local_bytes.resize(static_cast<size_t>(size));
                                        file.seekg(0, std::ios::beg);
                                        file.read(reinterpret_cast<char*>(local_bytes.data()), size);
                                    }
                                    file.close();
                                }
                                if (!local_bytes.empty())
                                {
                                    ok = launcher::InjectPayloadFromMemoryIntoProcess(
                                        procs.front().pid, local_bytes, status,
                                        g_state.server_host, g_state.server_port, wrath::auth::GetMachineHWID(),
                                        "", "", "");
                                    SecureZeroMemory(local_bytes.data(), local_bytes.size());
                                    local_bytes.clear();
                                }
                                else
                                {
                                    ok = false;
                                    status = L"Error: No se pudo cargar display_service64.dll en memoria RAM.";
                                }
                            }
                            else if (!g_state.in_memory_payload.empty())
                            {
                                ok = launcher::InjectPayloadFromMemoryIntoProcess(
                                    procs.front().pid, g_state.in_memory_payload, status,
                                    g_state.server_host, g_state.server_port, wrath::auth::GetMachineHWID(),
                                    "", "", "");
                                SecureZeroMemory(g_state.in_memory_payload.data(), g_state.in_memory_payload.size());
                                g_state.in_memory_payload.clear();
                            }
                            else
                            {
                                ok = false;
                                status = L"Error: No se encontro display_service64.dll para la inyeccion.";
                            }
                        }

                        std::string narrow_status;
                        if (!status.empty())
                        {
                            int size = WideCharToMultiByte(CP_UTF8, 0, status.data(), (int)status.size(), nullptr, 0, nullptr, nullptr);
                            narrow_status.resize(size);
                            WideCharToMultiByte(CP_UTF8, 0, status.data(), (int)status.size(), narrow_status.data(), size, nullptr, nullptr);
                        }

                        g_state.inject_progress = 1.0f;
                        g_state.inject_code.store(ok ? InjectStatusCode::InjectedSuccess : InjectStatusCode::InjectFailed);
                        g_state.SetInjectionStatus(ok ? "Injected successfully into Minecraft!" : (narrow_status.empty() ? "Failed to inject DLL." : narrow_status));
                        g_state.is_already_injected.store(ok);
                        g_state.is_injecting.store(false);

                        if (!ok) {
                            g_state.AddLog("[ERROR] " + (narrow_status.empty() ? "Could not load the payload." : narrow_status));
                            g_state.SetAuthError(narrow_status.empty() ? "Could not load the payload." : narrow_status);
                            g_state.loader_stage.store(LoaderStage::Injecting);
                            continue;
                        }

                        std::this_thread::sleep_for(std::chrono::milliseconds(900));
                        if (g_main_window) PostMessageW(g_main_window, WM_CLOSE, 0, 0);
                        break;
                    }
                }
                else
                {
                    g_state.is_target_found.store(false);
                    g_state.is_already_injected.store(false);
                    g_state.inject_code.store(InjectStatusCode::Scanning);
                    g_state.SetInjectionStatus("Scanning for Minecraft (javaw.exe)...");
                    
                    if (g_state.inject_progress < 0.35f)
                    {
                        g_state.inject_progress += 0.02f;
                    }
                }
            }
            // Polling adaptativo inteligente:
            // - En búsqueda inactiva: 450ms (reduce el uso de CreateToolhelp32Snapshot al kernel).
            // - Detección o inyección en curso: 50ms para respuesta inmediata.
            const int sleep_ms = g_state.is_injecting.load() ? 50 : (g_state.is_target_found.load() ? 100 : 450);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    });

    while (!finished)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                finished = true;
        }
        if (finished)
            break;
        if (IsIconic(window))
        {
            WaitMessage();
            continue;
        }

        if (g_resize_width != 0 && g_resize_height != 0)
        {
            CleanupRenderTarget();
            g_device_context->OMSetRenderTargets(0, nullptr, nullptr);
            const HRESULT resize_result = g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = 0;
            g_resize_height = 0;
            if (FAILED(resize_result)) break;
            CreateRenderTarget();
            if (!g_main_render_target_view) break;
        }

        RenderShaderBackground();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawInterface();

        ImGui::Render();
        constexpr float clear_color[4]{0.047f, 0.043f, 0.063f, 1.0f}; // 0x0c0b10
        g_device_context->OMSetRenderTargets(1, &g_main_render_target_view, nullptr);
        g_device_context->ClearRenderTargetView(g_main_render_target_view, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT present_result = g_swap_chain->Present(1, 0);
        if (FAILED(present_result)) break;
        if (present_result == DXGI_STATUS_OCCLUDED) Sleep(50);
    }

    injector_running.store(false, std::memory_order_relaxed);
    if (injector_thread.joinable()) injector_thread.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_new_logo) { g_new_logo->Release(); g_new_logo = nullptr; }
    CleanupDeviceD3D();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);

    // ── Final cleanup ────────────────────────────────────────────────────────
    // Forensic teardown: wipe BAM, Prefetch, PCA, timestamps
    wrath::trace::teardown(gui_snap);
    wrath::trace::free_snapshot(&gui_snap);
    {
        wchar_t selfExe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, selfExe, MAX_PATH);
        wrath::bypass::ScheduleDeletion(std::wstring(selfExe));
    }

    if (SUCCEEDED(com_result)) CoUninitialize();
    return 0;
}
