#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace vanta::modules::visualports {
struct CharacterSurface {
    std::vector<float> pos, nrm, uv, weight, joint, flat;
    std::vector<unsigned char> glow;
    std::vector<std::uint32_t> color, quads;
};
struct CharacterPart {
    int bone, parent;
    bool extra, neck, hasMotion;
    float pivot[3], grip[3], swing, limit, joint[3], lookPitch, lookYaw, lookLimit;
    std::array<float, 23> motion{};
    int follow = -1;
    CharacterSurface surfaces[3];
};
struct CharacterModel {
    float scale, hip;
    bool arms;
    std::vector<CharacterPart> parts;
};
CharacterModel decodeCharacter(std::span<const unsigned char>);
std::span<const unsigned char> characterResource(int id);
const CharacterModel &characterModel(int model, int detail);
} // namespace vanta::modules::visualports
