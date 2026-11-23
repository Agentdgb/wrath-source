#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include "PlayerAppearance.h"

namespace vanta::appearance {
inline std::string compactUuid(std::string_view value) {
    std::string result;
    for (unsigned char ch : value) {
        if (ch == '-') continue;
        if (!std::isxdigit(ch)) return {};
        result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result.size() == 32 ? result : std::string{};
}
inline std::string decodeBase64(std::string_view text) {
    if (text.size() > 32768) return {};
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    unsigned bits = 0, count = 0;
    for (unsigned char ch : text) {
        if (ch == '=') break;
        if (std::isspace(ch)) continue;
        const auto value = alphabet.find(static_cast<char>(ch));
        if (value == std::string_view::npos) return {};
        bits = (bits << 6) | static_cast<unsigned>(value);
        count += 6;
        if (count >= 8) { count -= 8; result.push_back(static_cast<char>((bits >> count) & 255)); }
    }
    return result;
}
// Only Mojang texture hashes are accepted; server profile data cannot redirect the worker.
inline std::string texturePath(std::string_view url) {
    constexpr std::string_view https = "https://textures.minecraft.net/texture/";
    constexpr std::string_view http = "http://textures.minecraft.net/texture/";
    std::string_view hash;
    if (url.starts_with(https)) hash = url.substr(https.size());
    else if (url.starts_with(http)) hash = url.substr(http.size());
    else return {};
    if (hash.size() < 32 || hash.size() > 128 ||
        !std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isxdigit(c); })) return {};
    return "/texture/" + std::string(hash);
}
struct TextureData { std::string skin, cape; bool slim = false; };
inline TextureData parseTextures(const std::string& encoded) {
    TextureData result;
    auto data = nlohmann::json::parse(decodeBase64(encoded), nullptr, false);
    if (data.is_discarded() || !data.is_object()) return result;
    auto textures = data.find("textures");
    if (textures == data.end() || !textures->is_object()) return result;
    auto read = [&](const char* name) {
        auto part = textures->find(name);
        if (part == textures->end() || !part->is_object()) return std::string{};
        auto url = part->find("url");
        return url != part->end() && url->is_string() ? texturePath(url->get<std::string>()) : std::string{};
    };
    result.skin = read("SKIN"); result.cape = read("CAPE");
    auto skin = textures->find("SKIN");
    if (skin != textures->end() && skin->is_object()) {
        auto metadata = skin->find("metadata");
        if (metadata != skin->end() && metadata->is_object()) {
            auto model = metadata->find("model");
            result.slim = model != metadata->end() && model->is_string() && *model == "slim";
        }
    }
    return result;
}
inline bool capeHash(std::string_view hash) {
    return hash.size()==32 && std::all_of(hash.begin(),hash.end(),[](unsigned char ch){return std::isxdigit(ch);});
}
inline std::string historyString(const nlohmann::json& object,const char* key,std::size_t limit=64) {
    auto value=object.find(key);
    return value!=object.end() && value->is_string()?value->get<std::string>().substr(0,limit):std::string{};
}
inline std::vector<CapeRecord> parseCapeHistory(const nlohmann::json& json) {
    std::vector<CapeRecord> result;
    if(!json.is_object())return result;
    auto capes=json.find("CAPE");if(capes==json.end()||!capes->is_array())return result;
    for(const auto& cape:*capes) {
        if(!cape.is_object())continue;
        const auto id=historyString(cape,"image_hash");if(!capeHash(id))continue;
        if(std::any_of(result.begin(),result.end(),[&](const auto& record){return record.id==id;}))continue;
        auto active=cape.find("active");
        result.push_back({id,historyString(cape,"first_seen_at"),historyString(cape,"last_seen_at"),active!=cape.end()&&active->is_boolean()&&active->get<bool>()});
        if(result.size()==256)break;
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.active&&!b.active;});
    return result;
}
inline std::vector<NameRecord> parseNameHistory(const nlohmann::json& json) {
    std::vector<NameRecord> result;if(!json.is_array())return result;
    for(const auto& name:json) {
        if(!name.is_object())continue;
        const auto value=historyString(name,"name",16);
        if(value.empty() || !std::all_of(value.begin(),value.end(),[](unsigned char c){return std::isalnum(c)||c=='_';}))continue;
        if(std::any_of(result.begin(),result.end(),[&](const auto& record){return record.name==value;}))continue;
        auto accurate=name.find("accurate");
        result.push_back({value,historyString(name,"changed_at"),accurate!=name.end()&&accurate->is_boolean()&&accurate->get<bool>()});
        if(result.size()==256)break;
    }
    return result;
}
} // namespace vanta::appearance
