#include "hitfx_render_context.h"

#include <mutex>

namespace vanta::modules::hitfx {

namespace {
    std::mutex   g_mx;
    render_frame g_latest{};
}

void publish_frame(const render_frame& f)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_latest = f;
}

bool latest_frame(render_frame& out)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_latest.valid) return false;
    out = g_latest;
    return true;
}

bool world_to_screen(const render_frame& f,
                     double world_x, double world_y, double world_z,
                     float& out_sx, float& out_sy, float& out_depth, bool clip_z)
{
    if (!f.valid || f.viewport_w <= 0 || f.viewport_h <= 0)
        return false;

    /* MVP = projection * modelview, column-major (matches Breeze's b():
       amounts3[i2+i*4] = sum_k projection[i2+k*4] * modelview[k+i*4]). With a
       flat column-major index [col*4 + row], C = A*B is
       C[col*4+row] = sum_k A[k*4+row] * B[col*4+k]. */
    float mvp[16];
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k)
                s += f.projection[k * 4 + row] * f.modelview[col * 4 + k];
            mvp[col * 4 + row] = s;
        }
    }

    // Camera-relative point (Breeze subtracts viewerPos before the multiply).
    const double rx = world_x - f.cam_x;
    const double ry = world_y - f.cam_y;
    const double rz = world_z - f.cam_z;

    // clip = MVP * (rx, ry, rz, 1). Reading row r as mvp[0*4+r], mvp[1*4+r], ...
    const float cx = static_cast<float>(mvp[0]  * rx + mvp[4]  * ry + mvp[8]  * rz + mvp[12]);
    const float cy = static_cast<float>(mvp[1]  * rx + mvp[5]  * ry + mvp[9]  * rz + mvp[13]);
    const float cz = static_cast<float>(mvp[2]  * rx + mvp[6]  * ry + mvp[10] * rz + mvp[14]);
    const float cw = static_cast<float>(mvp[3]  * rx + mvp[7]  * ry + mvp[11] * rz + mvp[15]);

    if (cw <= 0.0001f)
        return false;   // behind the camera

    const float ndc_x = cx / cw;
    const float ndc_y = cy / cw;
    const float ndc_z = cz / cw;

    if (clip_z && (ndc_z < -1.f || ndc_z > 1.f))
        return false;

    out_sx    = (ndc_x * 0.5f + 0.5f) * static_cast<float>(f.viewport_w);
    out_sy    = static_cast<float>(f.viewport_h) - (ndc_y * 0.5f + 0.5f) * static_cast<float>(f.viewport_h);
    out_depth = ndc_z;
    return true;
}

} // namespace vanta::modules::hitfx
