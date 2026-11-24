#pragma once

#include "focus_visual_logic.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vanta::modules::triggerbot::focus_visual {

// Geometry only: caller owns the draw list and clipping. This is shared by
// the actual world marker and the panel preview.
inline void draw(ImDrawList* dl, float left, float top, float right, float bottom,
                 float distance, int frame_style, int arrow_style,
                 bool show_arrow, bool show_distance)
{
    if (!dl || !std::isfinite(distance) || right <= left || bottom <= top) return;
    const ImU32 accent = IM_COL32(183, 108, 255, 245);
    const ImU32 shadow = IM_COL32(24, 12, 36, 200);
    auto segment = [&](float x1, float y1, float x2, float y2, float stroke = 2.f) {
        dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), shadow, stroke + 2.5f);
        dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), accent, stroke);
    };
    if (frame_style == 1) {
        dl->AddRect(ImVec2(left, top), ImVec2(right, bottom), shadow, 4.f, 0, 4.5f);
        dl->AddRect(ImVec2(left, top), ImVec2(right, bottom), accent, 4.f, 0, 2.f);
    } else if (frame_style == 0) {
        const float corner = std::clamp(std::min(right - left, bottom - top) * 0.22f, 7.f, 28.f);
        segment(left, top, left + corner, top); segment(left, top, left, top + corner);
        segment(right, top, right - corner, top); segment(right, top, right, top + corner);
        segment(left, bottom, left + corner, bottom); segment(left, bottom, left, bottom - corner);
        segment(right, bottom, right - corner, bottom); segment(right, bottom, right, bottom - corner);
    }

    const ImVec2 clip_min = dl->GetClipRectMin(), clip_max = dl->GetClipRectMax();
    const float arrow = arrow_size(distance);
    const float min_x = clip_min.x + arrow + 4.f, max_x = clip_max.x - arrow - 4.f;
    const float center = (left + right) * 0.5f;
    const float marker_x = min_x <= max_x ? std::clamp(center, min_x, max_x)
                                         : (clip_min.x + clip_max.x) * 0.5f;
    const float font_size = 16.f;
    const float arrow_height = arrow_style == 1 ? arrow * 1.1f : arrow * 0.8f;
    const float arrow_tip_y = std::max(top - 6.f, clip_min.y + arrow_height + (show_distance ? font_size + 10.f : 5.f));
    if (show_arrow) {
        if (arrow_style == 2) {
            ImVec2 points[] = {
                ImVec2(marker_x, arrow_tip_y),
                ImVec2(marker_x - arrow * 0.65f, arrow_tip_y - arrow * 0.40f),
                ImVec2(marker_x, arrow_tip_y - arrow * 0.80f),
                ImVec2(marker_x + arrow * 0.65f, arrow_tip_y - arrow * 0.40f)
            };
            dl->AddConvexPolyFilled(points, 4, shadow);
            dl->AddPolyline(points, 4, accent, ImDrawFlags_Closed, 2.4f);
            dl->AddCircleFilled(ImVec2(marker_x, arrow_tip_y - arrow * 0.40f),
                                std::max(2.f, arrow * 0.10f), IM_COL32(245, 226, 255, 240));
        } else {
            // Chevron: a slim luminous V, with an optional smaller V above it.
            segment(marker_x - arrow * 0.80f, arrow_tip_y - arrow * 0.60f,
                    marker_x, arrow_tip_y, 2.6f);
            segment(marker_x, arrow_tip_y, marker_x + arrow * 0.80f,
                    arrow_tip_y - arrow * 0.60f, 2.6f);
            if (arrow_style == 1) {
                const float y = arrow_tip_y - arrow * 0.42f;
                segment(marker_x - arrow * 0.68f, y - arrow * 0.50f,
                        marker_x, y, 2.f);
                segment(marker_x, y, marker_x + arrow * 0.68f,
                        y - arrow * 0.50f, 2.f);
            }
        }
    }
    if (show_distance) {
        char label[32]; std::snprintf(label, sizeof(label), "%.1f m", distance);
        ImFont* font = ImGui::GetFont();
        if (!font) return;
        const ImVec2 size = font->CalcTextSizeA(font_size, 1000.f, 0.f, label);
        const float x_lo = clip_min.x + 5.f, x_hi = clip_max.x - size.x - 5.f;
        const float label_x = x_lo <= x_hi ? std::clamp(marker_x - size.x * 0.5f, x_lo, x_hi) : x_lo;
        const float above = show_arrow ? arrow_tip_y - arrow_height : top - 5.f;
        const float label_y = std::max(clip_min.y + 4.f, above - size.y - 5.f);
        dl->AddRectFilled(ImVec2(label_x - 5.f, label_y - 2.f),
                          ImVec2(label_x + size.x + 5.f, label_y + size.y + 2.f),
                          IM_COL32(18, 15, 25, 205), 4.f);
        dl->AddText(font, font_size, ImVec2(label_x, label_y), IM_COL32(255, 255, 255, 255), label);
    }
}
}
