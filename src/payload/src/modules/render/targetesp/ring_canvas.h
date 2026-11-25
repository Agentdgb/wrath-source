#pragma once
#include "imgui.h"

namespace dt::modules::targetesp {
// gl_hook transforms and fades every list except ##Foreground with the menu.
// World overlays use framebuffer coordinates, while ImGui's default clip is
// the 1120x700 menu canvas. Override that clip without intersecting it.
class RingCanvas {
public:
    ImDrawList* draw;
    RingCanvas(int width, int height) : draw(ImGui::GetForegroundDrawList()) {
        draw->PushClipRect(ImVec2(0, 0), ImVec2(float(width), float(height)), false);
    }
    ~RingCanvas() { draw->PopClipRect(); }
    RingCanvas(const RingCanvas&) = delete;
    RingCanvas& operator=(const RingCanvas&) = delete;
};
}
