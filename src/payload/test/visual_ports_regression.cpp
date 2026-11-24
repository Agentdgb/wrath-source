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
        std::cout << "Visual math passed: six swing styles, mirrored hands, finite transforms and rest endpoints.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
