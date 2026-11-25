#pragma once
#include <array>
#include <cmath>
namespace dt::modules::spotify {
// Slow independent waves bend the artwork without exposing texture edges.
inline std::array<float,2> ember_uv(float u,float v,double seconds) {
    const double t=seconds*.50;
    return {
        float(.18+.64*u+.110*std::sin(v*4.2+t)+.045*std::sin(u*3.1-v*2.2-t*.73)),
        float(.18+.64*v+.110*std::sin(u*4.1-t*.82)+.045*std::cos(v*3.6+u*1.8+t*.61))
    };
}
}
