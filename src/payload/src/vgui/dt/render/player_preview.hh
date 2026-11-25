#pragma once
#include "imgui.h"
#include "core/profile/PlayerAppearance.h"
namespace dt {
void draw_player_preview(ImDrawList* draw, const appearance::Snapshot& player,
                         float x, float y, float w, float h, float yaw, bool layers, bool cape);
}
