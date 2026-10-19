#pragma once
#include <iterator>
#include <string_view>
namespace vanta::modules::swordreplace {
inline constexpr const char* models[] = {
    "Katana", "Cyber Katana", "Amethyst Shuriken", "Yoru", "True Excalibur",
    "Terra Blade", "Rivers Of Blood", "Soul Edge", "Frost Blade", "Emberblade",
    "Molten Blade", "Dark Blade", "Blood Edge", "Demonic Blade", "Thunderbrand",
    "Masamune", "Muramasa", "Longsword", "Royal Rapier", "Abominable Blade",
};
inline constexpr int defaultModel = 0;
static_assert(std::size(models) == 20);
void update(bool enabled, int model);
const char* status();
bool prepareShutdown();
void shutdownCache();
struct Asset { const char* path; const unsigned char* bytes; unsigned size; };
extern const Asset assets[];
extern const unsigned assetCount;
extern const char* const modelPaths[];
const Asset* findAsset(std::string_view path);
}
