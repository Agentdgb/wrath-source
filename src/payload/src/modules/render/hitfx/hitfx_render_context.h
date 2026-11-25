// hitfx_render_context.h — the captured camera frame + world->screen projection.
//
// The capture hook (hitfx_capture_hook) publishes the game's real modelview
// (positionMatrix) + projection matrices, the camera world position, and the
// viewport into a single mutex-guarded RenderContext each world-render frame.
// The hitfx render pass, running on the wglSwapBuffers thread, reads the
// latest frame and calls world_to_screen() — a faithful port of Breeze's
// MinecraftAccessor13.a() gluProject: MVP = projection * modelview (column
// major), clip = MVP * (worldPos - camPos, 1), ndc = clip.xyz / clip.w,
// screenX = (ndc.x*0.5+0.5)*vw, screenY = vh - (ndc.y*0.5+0.5)*vh, cull when
// clip.w <= 0 or |ndc.z| > 1.

#pragma once

namespace dt::modules::hitfx {

// One captured world-render frame. Matrices are column-major (org.joml
// Matrix4f.get(float[]) layout, GL-compatible).
struct render_frame {
    float  modelview[16] = {};   // camera rotation ONLY — no view bob / hurt tilt; fold them
                                 // in with overlay_view::apply_view_bob before projecting
    float  projection[16] = {};
    double cam_x = 0.0, cam_y = 0.0, cam_z = 0.0;
    int    viewport_w = 0, viewport_h = 0;
    bool   valid = false;
    unsigned long long frame = 0;   // increments each publish, lets consumers detect staleness
};

// Publish a freshly captured frame (called from the capture hook detour on the
// render thread). Copies under a mutex; cheap.
void publish_frame(const render_frame& f);

// Snapshot the latest published frame (called from the render pass). Returns
// false if nothing has been captured yet.
bool latest_frame(render_frame& out);

// gluProject. world_x/y/z are ABSOLUTE world coordinates; the function
// subtracts the captured camera position internally (camera-relative), exactly
// like Breeze. Writes framebuffer-pixel screen coords to out_sx/out_sy and the
// NDC depth to out_depth. Returns false (and writes nothing usable) when the
// point is behind the camera or outside the depth range. clip_z=false lets
// volume renderers recover edges across the depth planes; behind-camera points
// are always rejected. Existing point-anchor callers keep depth clipping.
bool world_to_screen(const render_frame& f,
                     double world_x, double world_y, double world_z,
                     float& out_sx, float& out_sy, float& out_depth, bool clip_z = true);

} // namespace dt::modules::hitfx
