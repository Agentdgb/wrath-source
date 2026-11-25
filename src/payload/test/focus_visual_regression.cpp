#include "modules/combat/triggerbot/focus_visual_draw.h"
#include <cmath>
#include <iostream>

float alpha_text = 1.f;
using namespace dt::modules::triggerbot::focus_visual;
int main() {
    if (!expired(31000, 1000, 30.f) || expired(30999, 1000, 30.f) || expired(99999, 1000, 0.f)) return 1;
    if (!(arrow_size(1) < arrow_size(3) && arrow_size(3) < arrow_size(10))) return 2;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.DisplaySize = ImVec2(800, 600); io.DeltaTime = 1.f/60.f;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1);
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(ImVec2(0, 0), ImVec2(800, 600), false);
    for (int frame = 0; frame < 3; ++frame) {
        for (int arrow = 0; arrow < 3; ++arrow) {
            const int old = dl->VtxBuffer.Size;
            draw(dl, 300, 180, 500, 500, 6.f, frame, arrow, true, true);
            if (dl->VtxBuffer.Size <= old) return 3;
            for (int i = old; i < dl->VtxBuffer.Size; ++i) {
                const ImVec2 p = dl->VtxBuffer[i].pos;
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) return 4;
            }
        }
    }
    const int old = dl->VtxBuffer.Size;
    draw(dl, 300, 180, 500, 500, 6.f, 2, 0, false, false);
    if (dl->VtxBuffer.Size != old) return 5;
    dl->PopClipRect();
    ImGui::EndFrame();
    ImGui::DestroyContext();
    std::cout << "Focus visual timer, scaling and 9 style combinations: OK\n";
    return 0;
}
