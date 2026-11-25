#include "PlayerAppearance.h"
#include "AppearanceData.h"
#include "platform/ThreadUtil.h"
#include "vgui/framework/render/stb/stb_image.hh"
#include <windows.h>
#include <winhttp.h>
#include <gl/GL.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dt::appearance {
namespace {
struct Image { std::vector<unsigned char> pixels; int w = 0, h = 0; unsigned texture = 0; };
struct CapeRender { Image image; bool pending=true; std::uint64_t used=0; std::chrono::steady_clock::time_point retryAt{}; };
struct Entry {
    Snapshot info; Image skin, cape; std::string source;
    std::uint64_t used = 0, version = 0; bool pending = true;
    std::chrono::steady_clock::time_point retryAt{};
    std::unordered_map<std::string,CapeRender> capeRenders;
};
struct Job { std::string key, uuid, textures; std::uint64_t version = 0; std::string historyCape; };
std::mutex mutex;
std::condition_variable wake;
std::unordered_map<std::string, Entry> entries;
std::deque<Job> jobs;
std::atomic<bool> running{false}, stopped{false};
std::uint64_t clock = 0, serial = 0;
std::string selected;
struct Http { HINTERNET value; ~Http() { if (value) WinHttpCloseHandle(value); } };
bool http(const wchar_t* host, const std::string& path, std::vector<unsigned char>& result) {
    if (!running.load()) return false;
    Http session{WinHttpOpen(L"Wrath/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
    if (!session.value) return false;
    WinHttpSetTimeouts(session.value, 2000, 2000, 2000, 2000);
    Http connection{WinHttpConnect(session.value, host, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.value) return false;
    const std::wstring wide(path.begin(), path.end());
    Http request{WinHttpOpenRequest(connection.value, L"GET", wide.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE)};
    if (!request.value) return false;
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    if (!WinHttpSendRequest(request.value, nullptr, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) return false;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             nullptr, &status, &size, nullptr) || status != 200) return false;
    while (running.load()) {
        DWORD available = 0, read = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) return false;
        if (!available) return !result.empty();
        if (result.size() + available > 512 * 1024) return false;
        const auto offset = result.size(); result.resize(offset + available);
        if (!WinHttpReadData(request.value, result.data() + offset, available, &read) || !read) return false;
        result.resize(offset + read);
    }
    return false;
}
bool image(const std::string& path, Image& out, bool skin) {
    if (path.empty()) return false;
    std::vector<unsigned char> png;
    if (!http(L"textures.minecraft.net", path, png)) return false;
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels) ||
        w < 64 || w > 1024 || (h != w && h * 2 != w) || (skin && w % 64) || (!skin && h * 2 != w)) return false;
    auto* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, nullptr, 4);
    if (!pixels) return false;
    out.w = w; out.h = h; out.pixels.assign(pixels, pixels + w * h * 4); stbi_image_free(pixels);
    return true;
}
bool capeRender(const std::string& uuid,const std::string& hash,Image& out) {
    if(compactUuid(uuid).empty()||!capeHash(hash))return false;
    std::vector<unsigned char> png;
    if(!http(L"laby.net","/api/v3/render/player/"+uuid+".png?cape="+hash+"&pose=back&width=256&height=256",png))return false;
    int w=0,h=0,channels=0;
    if(!stbi_info_from_memory(png.data(),static_cast<int>(png.size()),&w,&h,&channels)||w!=256||h!=256)return false;
    auto* pixels=stbi_load_from_memory(png.data(),static_cast<int>(png.size()),&w,&h,nullptr,4);
    if(!pixels)return false;
    out.w=w;out.h=h;out.pixels.assign(pixels,pixels+w*h*4);stbi_image_free(pixels);return true;
}
Entry fetch(const Job& job) {
    Entry result;
    std::string encoded = job.textures;
    if (encoded.empty() && !job.uuid.empty()) {
        std::vector<unsigned char> response;
        if (http(L"sessionserver.mojang.com", "/session/minecraft/profile/" + job.uuid, response)) {
            auto data = nlohmann::json::parse(response.begin(), response.end(), nullptr, false);
            if (data.is_object() && data.contains("properties") && data["properties"].is_array())
                for (const auto& property : data["properties"])
                    if (property.is_object() && property.value("name", std::string{}) == "textures" &&
                        property.contains("value") && property["value"].is_string()) {
                        encoded = property["value"].get<std::string>(); break;
                    }
        }
    }
    const auto textures = parseTextures(encoded);
    result.info.slim = textures.slim;
    result.info.hasCape = !textures.cape.empty();
    const bool skinOk = image(textures.skin, result.skin, true);
    const bool capeOk = image(textures.cape, result.cape, false);
    result.info.legacy = result.skin.h * 2 == result.skin.w;
    result.info.state = skinOk ? State::ready : State::unavailable;
    result.info.message = skinOk ? (result.info.hasCape ? (capeOk ? "Skin and cape loaded" : "Cape texture unavailable") : "No cape equipped")
                                : "Skin unavailable for this profile";
    if(!job.uuid.empty()) {
        std::vector<unsigned char> response;
        if(http(L"laby.net","/api/v3/user/"+job.uuid+"/textures",response)) {
            auto json=nlohmann::json::parse(response.begin(),response.end(),nullptr,false);
            result.info.capeHistoryAvailable=json.is_object()&&(json.contains("CAPE")||json.contains("SKIN")||json.contains("BANDANA"));
            result.info.capes=parseCapeHistory(json);
        }
        response.clear();
        if(http(L"laby.net","/api/v3/user/"+job.uuid+"/names",response)) {
            auto json=nlohmann::json::parse(response.begin(),response.end(),nullptr,false);
            result.info.nameHistoryAvailable=json.is_array();
            result.info.names=parseNameHistory(json);
        }
    }
    result.pending = false;
    return result;
}
void worker() {
    while (running.load()) {
        Job job;
        { std::unique_lock lock(mutex); wake.wait(lock, [] { return !running.load() || !jobs.empty(); });
          if (!running.load()) return; job = std::move(jobs.front()); jobs.pop_front(); }
        if(!job.historyCape.empty()) {
            Image rendered;
            try { capeRender(job.uuid,job.historyCape,rendered); } catch(...) {}
            std::lock_guard lock(mutex);
            auto found=entries.find(job.key);
            if(running.load() && found!=entries.end() && found->second.version==job.version) {
                auto cape=found->second.capeRenders.find(job.historyCape);
                if(cape!=found->second.capeRenders.end()) {
                    cape->second.image=std::move(rendered);cape->second.pending=false;
                    cape->second.retryAt=std::chrono::steady_clock::now()+std::chrono::seconds(30);
                }
            }
            continue;
        }
        Entry result;
        try { result = fetch(job); }
        catch (...) { result.pending = false; result.info.state = State::unavailable; result.info.message = "Profile data unavailable"; }
        result.source = job.textures;
        result.retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::lock_guard lock(mutex);
        auto found = entries.find(job.key);
        if (running.load() && found != entries.end() && found->second.version == job.version) {
            result.used = found->second.used; result.version = job.version;
            found->second = std::move(result);
        }
    }
}
void destroy(Entry& entry) {
    if (entry.skin.texture) glDeleteTextures(1, &entry.skin.texture);
    if (entry.cape.texture) glDeleteTextures(1, &entry.cape.texture);
    for(auto& [id,cape]:entry.capeRenders)if(cape.image.texture)glDeleteTextures(1,&cape.image.texture);
}
void upload(Image& image) {
    if (image.texture || image.pixels.empty()) return;
    GLint previous = 0, unpack = 0, alignment = 0, row = 0, rows = 0, pixels = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous); glGetIntegerv(0x88EF, &unpack);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment); glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &rows); glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &pixels);
    using BindBuffer = void(APIENTRY*)(GLenum, GLuint);
    auto bind = reinterpret_cast<BindBuffer>(wglGetProcAddress("glBindBuffer"));
    const auto valid = reinterpret_cast<std::intptr_t>(bind);
    if (valid <= 3 || valid == -1) return;
    bind(0x88EC, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glGenTextures(1, &image.texture); glBindTexture(GL_TEXTURE_2D, image.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.w, image.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels.data());
    glBindTexture(GL_TEXTURE_2D, previous); bind(0x88EC, unpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment); glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, rows); glPixelStorei(GL_UNPACK_SKIP_PIXELS, pixels);
    image.pixels.clear();
}
} // namespace
void select(const std::string& name) { selected = name; }
const std::string& selectedName() { return selected; }
Snapshot get(const std::string& name, const std::string& uuid, const std::string& textures, bool detailsReady, const std::string& historyCape) {
    if (stopped.load()) return {State::unavailable, 0, 0, false, false, false, "Preview stopped"};
    if (!detailsReady) return {};
    const auto id = compactUuid(uuid);
    const std::string key = id.empty() ? name : id;
    std::lock_guard lock(mutex);
    auto found = entries.find(key);
    if (found != entries.end() && ((!textures.empty() && found->second.source != textures) ||
        (!found->second.pending && (found->second.info.state == State::unavailable ||
         !found->second.info.capeHistoryAvailable || !found->second.info.nameHistoryAvailable) &&
         std::chrono::steady_clock::now() > found->second.retryAt))) {
        destroy(found->second); entries.erase(found); found = entries.end();
        std::erase_if(jobs, [&](const Job& job) { return job.key == key; });
    }
    if (found == entries.end()) {
        if (entries.size() >= 8) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (!it->second.pending && (oldest == entries.end() || it->second.used < oldest->second.used)) oldest = it;
            if (oldest == entries.end()) return {};
            destroy(oldest->second); entries.erase(oldest);
        }
        if (!running.exchange(true)) {
            if (!platform::spawnNamed("Netty-IO-2", &worker)) {
                running.store(false);
                return {State::unavailable, 0, 0, false, false, false, "Profile worker unavailable"};
            }
        }
        Entry entry; entry.version = ++serial; entry.source = textures.substr(0, 32768);
        entry.info.message = "Loading skin and cape...";
        found = entries.emplace(key, std::move(entry)).first;
        jobs.push_back({key, id, textures.substr(0, 32768), found->second.version}); wake.notify_one();
    }
    auto& entry = found->second; entry.used = ++clock;
    if (!entry.pending) { upload(entry.skin); upload(entry.cape); }
    entry.info.skin = entry.skin.texture; entry.info.cape = entry.cape.texture;
    entry.info.historicalCapeTexture=0;entry.info.historicalCapeLoading=false;
    const bool recorded=std::any_of(entry.info.capes.begin(),entry.info.capes.end(),[&](const auto& cape){return cape.id==historyCape;});
    if(!entry.pending && recorded) {
        auto cape=entry.capeRenders.find(historyCape);
        if(cape!=entry.capeRenders.end()&&!cape->second.pending&&!cape->second.image.texture&&
           cape->second.image.pixels.empty()&&std::chrono::steady_clock::now()>cape->second.retryAt) {
            entry.capeRenders.erase(cape);cape=entry.capeRenders.end();
        }
        if(cape==entry.capeRenders.end()) {
            if(entry.capeRenders.size()>=4) {
                auto oldest=entry.capeRenders.end();
                for(auto it=entry.capeRenders.begin();it!=entry.capeRenders.end();++it)
                    if(!it->second.pending && (oldest==entry.capeRenders.end()||it->second.used<oldest->second.used))oldest=it;
                if(oldest!=entry.capeRenders.end()) {if(oldest->second.image.texture)glDeleteTextures(1,&oldest->second.image.texture);entry.capeRenders.erase(oldest);}
            }
            if(entry.capeRenders.size()<4) {
                cape=entry.capeRenders.emplace(historyCape,CapeRender{}).first;
                jobs.push_back({key,id,{},entry.version,historyCape});wake.notify_one();
            }
        }
        if(cape!=entry.capeRenders.end()) {
            cape->second.used=clock;
            if(!cape->second.pending)upload(cape->second.image);
            entry.info.historicalCapeTexture=cape->second.image.texture;
            entry.info.historicalCapeLoading=cape->second.pending;
        }
    }
    return entry.info;
}
void shutdown() {
    stopped.store(true);
    { std::lock_guard lock(mutex); running.store(false); for (auto& [key, entry] : entries) destroy(entry); entries.clear(); jobs.clear(); }
    wake.notify_all();
}
} // namespace dt::appearance
