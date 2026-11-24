#pragma once
#include "imgui.h"

namespace vanta::modules {
// Foreground overlays share a draw list but may use different font atlases.
// Geometry must use the white pixel's atlas and geometric line antialiasing.
class WorldOverlayDrawScope {
    ImDrawList* list_;
    ImDrawListFlags flags_;
public:
    explicit WorldOverlayDrawScope(ImDrawList* list) : list_(list), flags_(list->Flags) {
        list_->PushTextureID(ImGui::GetIO().Fonts->TexID);
        list_->Flags &= ~ImDrawListFlags_AntiAliasedLinesUseTex;
    }
    ~WorldOverlayDrawScope() {
        list_->Flags = flags_;
        list_->PopTextureID();
    }
    WorldOverlayDrawScope(const WorldOverlayDrawScope&) = delete;
    WorldOverlayDrawScope& operator=(const WorldOverlayDrawScope&) = delete;
};
}
