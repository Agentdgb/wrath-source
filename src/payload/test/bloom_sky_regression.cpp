#include "modules/visuals/bloom/native/NativeImages.h"
#include "modules/visuals/bloom/native/SkyModel.h"
#include <iostream>
using namespace dt::modules::bloom;
int main() {
    try {
        sky::starData(span::q(7231977), span::r(7231977));
        std::size_t vertices = 0;
        bool galaxy = false, aurora = false;
        for (int preset = 0; preset < 13; ++preset) {
            sky::reset();
            sky::State state;
            state.pw[preset] = 1;
            state.cv = .9f;
            state.ep = 1;
            sky::profile(state.cs, preset, .55f);
            state.er = state.cs[19];
            state.es = state.cs[20];
            for (int step = 0; step < 8; ++step) {
                sky::Input input;
                input.forward = {0, .6f, -.8f};
                input.up = {0, .8f, .6f};
                state.dd = step * .1;
                sky::Buffers buffers;
                sky::generate(state, input, buffers);
                for (auto &stream : buffers.streams) {
                    if (stream.vertices.size() % 4)
                        throw std::runtime_error("Sky quad stream is incomplete");
                    if (stream.layer < 0 || stream.layer > 7)
                        throw std::runtime_error("Invalid sky layer");
                    for (auto &v : stream.vertices)
                        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.u) ||
                            !std::isfinite(v.v))
                            throw std::runtime_error("Invalid sky vertex");
                    vertices += stream.vertices.size();
                    if (preset == 3 && !stream.vertices.empty())
                        galaxy = true;
                    if (preset == 4 && !stream.vertices.empty())
                        aurora = true;
                }
            }
        }
        if (!galaxy || !aurora || vertices < 1000)
            throw std::runtime_error("Missing sky geometry");
        sky::reset();
        std::cout << "Sky profiles passed: 13 presets, " << vertices
                  << " vertices, finite geometry, valid layers and quad balance.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
