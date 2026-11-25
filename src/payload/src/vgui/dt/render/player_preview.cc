#include "player_preview.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace dt {
void draw_player_preview(ImDrawList* draw, const appearance::Snapshot& player,
                         float x, float y, float w, float h, float yaw, bool layers, bool cape) {
    if (!draw || !player.skin || w <= 0 || h <= 0) return;
    struct Point { float x, y, z; };
    struct Face { std::array<ImVec2, 4> p, uv; float depth; ImTextureID texture; ImU32 shade; };
    std::vector<Face> faces; faces.reserve(48);
    const float cs = std::cos(yaw), sn = std::sin(yaw);
    const float scale = std::min(w / 24.f, h / 36.f);
    auto box = [&](float cx, float cy, float cz, float bw, float bh, float bd, int u, int v,
                   unsigned texture, float textureH, float inflate, bool mirror = false) {
        auto point = [&](float a, float b, float c) { return Point{cx + a * (bw / 2 + inflate), cy + b * (bh / 2 + inflate), cz + c * (bd / 2 + inflate)}; };
        auto face = [&](Point normal, std::array<Point, 4> corners, float tu, float tv, float tw, float th, float shade) {
            if (normal.x * sn + normal.z * cs - normal.y * .16f >= 0) return;
            Face f{}; f.texture = static_cast<ImTextureID>(texture); f.shade = IM_COL32(int(shade * 255), int(shade * 255), int(shade * 255), 255);
            for (int i = 0; i < 4; ++i) {
                const auto p = corners[i]; const float px = p.x * cs - p.z * sn, depth = p.x * sn + p.z * cs;
                f.p[i] = {x + w * .5f + px * scale, y + h * .5f - (p.y - 16) * scale + depth * scale * .16f};
                f.depth += depth;
                const bool right = i == 1 || i == 2;
                f.uv[i] = {(tu + ((right != mirror) ? tw : 0)) / 64.f, (tv + (i >= 2 ? th : 0)) / textureH};
            }
            faces.push_back(f);
        };
        face({0,0,-1}, {point(-1,1,-1),point(1,1,-1),point(1,-1,-1),point(-1,-1,-1)}, u+bd,v+bd,bw,bh,1);
        face({0,0,1}, {point(1,1,1),point(-1,1,1),point(-1,-1,1),point(1,-1,1)}, u+bd+bw+bd,v+bd,bw,bh,.86f);
        face({-1,0,0}, {point(-1,1,1),point(-1,1,-1),point(-1,-1,-1),point(-1,-1,1)}, u,v+bd,bd,bh,.75f);
        face({1,0,0}, {point(1,1,-1),point(1,1,1),point(1,-1,1),point(1,-1,-1)}, u+bd+bw,v+bd,bd,bh,.82f);
        face({0,1,0}, {point(-1,1,1),point(1,1,1),point(1,1,-1),point(-1,1,-1)}, u+bd,v,bw,bd,1);
        face({0,-1,0}, {point(-1,-1,-1),point(1,-1,-1),point(1,-1,1),point(-1,-1,1)}, u+bd+bw,v,bw,bd,.65f);
    };
    const float skinH = player.legacy ? 32.f : 64.f;
    auto model = [&](bool overlay) {
        const float inflate = overlay ? .25f : 0;
        const int arm = player.slim ? 3 : 4;
        box(0,28,0,8,8,8,overlay?32:0,0,player.skin,skinH,overlay?.5f:0);
        box(0,18,0,8,12,4,16,overlay?32:16,player.skin,skinH,inflate);
        box(-4-arm*.5f,18,0,float(arm),12,4,40,overlay?32:16,player.skin,skinH,inflate);
        box(4+arm*.5f,18,0,float(arm),12,4,player.legacy?40:(overlay?48:32),player.legacy?16:48,player.skin,skinH,inflate,player.legacy);
        box(-2,6,0,4,12,4,0,overlay?32:16,player.skin,skinH,inflate);
        box(2,6,0,4,12,4,player.legacy?0:(overlay?0:16),player.legacy?16:48,player.skin,skinH,inflate,player.legacy);
    };
    model(false);
    if (layers && !player.legacy) model(true);
    else if (layers) box(0,28,0,8,8,8,32,0,player.skin,skinH,.5f);
    if (cape && player.cape) box(0,16,2.8f,10,16,1,0,0,player.cape,32,.05f);
    std::stable_sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.depth > b.depth; });
    draw->PushClipRect({x,y},{x+w,y+h},true);
    for (const auto& f : faces) draw->AddImageQuad(f.texture, f.p[0], f.p[1], f.p[2], f.p[3], f.uv[0], f.uv[1], f.uv[2], f.uv[3], f.shade);
    draw->PopClipRect();
}
} // namespace dt
