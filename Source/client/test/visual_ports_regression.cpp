#include "modules/visuals/ports/CharacterData.h"
#include "modules/visuals/ports/VisualPorts.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace vanta::modules::visualports;
void require(bool test, const char *why) {
    if (!test)
        throw std::runtime_error(why);
}
int main() {
    try {
        for (int style = 0; style < 6; ++style) {
            require(math::swingPose(style, 0, 1, 1).empty(), "Swing moves at rest");
            require(math::swingPose(style, 1, 1, 1).empty(), "Swing does not return to rest");
            for (int i = 1; i < 100; ++i) {
                auto right = math::swingPose(style, i / 100.f, 1, 2), left = math::swingPose(style, i / 100.f, -1, 2);
                require(right.size() == left.size(), "Hands have mismatched operations");
                for (std::size_t n = 0; n < right.size(); ++n) {
                    auto a = right[n], b = left[n];
                    require(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z), "Invalid swing transform");
                    if (a.axis == -1)
                        require(std::abs(a.x + b.x) < 1e-5f && a.y == b.y && a.z == b.z, "Translation mirror mismatch");
                    else if (a.axis == 0)
                        require(a.x == b.x, "Pitch mirror mismatch");
                    else
                        require(std::abs(a.y + b.y) + std::abs(a.z + b.z) < 1e-5f, "Rotation mirror mismatch");
                }
            }
        }
        auto a = math::retention(.5f, 1. / 60), b = math::retention(.5f, 1. / 120);
        require(std::abs(a - b * b) < 1e-6f, "Motion blur depends on FPS");
        require(math::retention(.5f, 2) == 0, "Stalled frame retains history");
        require(math::ascend(0) == 0 && std::abs(math::ascend(1) - 1) < 1e-5f, "Spirit rise endpoints");
        require(math::dissolve(.4f, 0) == 1 && math::dissolve(1, 1) == 0, "Spirit dissolve endpoints");
        auto pose = math::spiritPose(0, 0, 0, 0, false, 0);
        std::array<std::array<float, 3>, 8> v{};
        math::spiritCorners(5, 0, pose, false, 0, .05859375f, 0, v);
        float top = 0;
        for (auto &p : v)
            top = std::max(top, p[1]);
        require(std::abs(top - 1.875f) < 1e-6f, "Spirit model scale differs from source");
        std::size_t vertices = 0;
        for (int m = 0; m < 7; ++m)
            for (int d = 0; d < 3; ++d) {
                const auto &model = characterModel(m, d);
                require(!model.parts.empty() && model.scale > 0, "Empty character");
                for (auto &p : model.parts)
                    for (auto &s : p.surfaces)
                        vertices += s.pos.size() / 3;
                auto bytes = characterResource(2000 + m * 3 + d);
                bool rejected = false;
                try {
                    decodeCharacter(bytes.first(bytes.size() / 2));
                } catch (...) {
                    rejected = true;
                }
                require(rejected, "Truncated model accepted");
                require(!characterResource(2100 + m).empty(), "Missing character texture");
            }
        require(vertices > 100000, "Incomplete character meshes");
        std::cout << "Visual ports passed: six styles, temporal retention, spirit geometry, 21 meshes (" << vertices
                  << " vertices).\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
