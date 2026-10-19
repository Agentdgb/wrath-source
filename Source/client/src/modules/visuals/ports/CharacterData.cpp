#include "CharacterData.h"
#include <bit>
#include <memory>
#include <stdexcept>
#include <windows.h>
namespace vanta::modules::visualports {
namespace {
struct Reader {
    std::span<const unsigned char> s;
    std::size_t at = 0;
    std::uint32_t u() {
        if (at + 4 > s.size())
            throw std::runtime_error("Truncated character model");
        auto v = (std::uint32_t(s[at]) << 24) | (std::uint32_t(s[at + 1]) << 16) | (std::uint32_t(s[at + 2]) << 8) |
                 s[at + 3];
        at += 4;
        return v;
    }
    bool b() {
        if (at >= s.size())
            throw std::runtime_error("Truncated character flag");
        return s[at++] != 0;
    }
    float f() {
        return std::bit_cast<float>(u());
    }
    std::vector<float> floats() {
        auto n = u();
        if (n > 2000000 || std::size_t(n) * 4 > s.size() - at)
            throw std::runtime_error("Invalid character vertices");
        std::vector<float> v(n);
        for (auto &x : v)
            x = f();
        return v;
    }
    std::vector<std::uint32_t> ints() {
        auto n = u();
        if (n > 2000000 || std::size_t(n) * 4 > s.size() - at)
            throw std::runtime_error("Invalid character indices");
        std::vector<std::uint32_t> v(n);
        for (auto &x : v)
            x = u();
        return v;
    }
};
} // namespace
CharacterModel decodeCharacter(std::span<const unsigned char> bytes) {
    Reader r{bytes};
    if (r.u() != 0x57434832)
        throw std::runtime_error("Unknown character format");
    CharacterModel m{};
    m.scale = r.f();
    m.hip = r.f();
    m.arms = r.b();
    auto count = r.u();
    if (count > 512)
        throw std::runtime_error("Invalid character part count");
    m.parts.resize(count);
    for (unsigned i = 0; i < count; ++i) {
        auto &p = m.parts[i];
        p.bone = static_cast<int>(r.u());
        p.parent = static_cast<int>(r.u());
        p.extra = r.b();
        if (p.bone < 0 || p.bone >= 6 || (p.extra && (p.parent < 0 || p.parent >= static_cast<int>(i))))
            throw std::runtime_error("Invalid character skeleton");
        for (auto &x : p.pivot)
            x = r.f();
        for (auto &x : p.grip)
            x = r.f();
        p.swing = r.f();
        p.limit = r.f();
        for (auto &x : p.joint)
            x = r.f();
        p.neck = r.b();
        p.lookPitch = r.f();
        p.lookYaw = r.f();
        p.lookLimit = r.f();
        p.hasMotion = r.b();
        if (p.hasMotion) {
            for (auto &x : p.motion)
                x = r.f();
            p.follow = static_cast<int>(r.u());
        }
        for (auto &s : p.surfaces)
            if (r.b()) {
                s.pos = r.floats();
                s.nrm = r.floats();
                s.uv = r.floats();
                s.color = r.ints();
                s.quads = r.ints();
                s.weight = r.floats();
                s.joint = r.floats();
                auto gn = r.u();
                if (gn > r.s.size() - r.at)
                    throw std::runtime_error("Invalid character glow data");
                s.glow.assign(r.s.begin() + r.at, r.s.begin() + r.at + gn);
                r.at += gn;
                s.flat = r.floats();
                auto n = s.pos.size() / 3;
                if (s.pos.size() % 3 || s.nrm.size() != s.pos.size() || s.uv.size() != n * 2 || s.color.size() != n ||
                    (!s.weight.empty() && s.weight.size() != n) || (!s.joint.empty() && s.joint.size() != n) ||
                    s.quads.size() % 4 || (!s.glow.empty() && s.glow.size() != n) ||
                    (!s.flat.empty() && s.flat.size() != n))
                    throw std::runtime_error("Invalid character surface");
                for (auto q : s.quads)
                    if (q >= n)
                        throw std::runtime_error("Character index outside surface");
            }
    }
    if (r.at != bytes.size())
        throw std::runtime_error("Unexpected character data");
    return m;
}
std::span<const unsigned char> characterResource(int id) {
    HMODULE image = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&characterResource), &image))
        return {};
    auto r = FindResourceW(image, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r)
        return {};
    auto data = LoadResource(image, r);
    return {static_cast<const unsigned char *>(LockResource(data)), SizeofResource(image, r)};
}
const CharacterModel &characterModel(int model, int detail) {
    static std::unique_ptr<CharacterModel> cache[7][3];
    if (model < 0 || model >= 7 || detail < 0 || detail >= 3)
        throw std::runtime_error("Invalid character selection");
    auto &m = cache[model][detail];
    if (!m)
        m = std::make_unique<CharacterModel>(decodeCharacter(characterResource(2000 + model * 3 + detail)));
    return *m;
}
} // namespace vanta::modules::visualports
