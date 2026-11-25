#include "core/setting/KeybindSetting.h"

#include <imgui.h>
#include <windows.h>
#include <string>

namespace dt {

namespace {

std::string vkName(int vk) {
    if (vk == 0) return "NONE";
    if (vk == VK_RBUTTON) return "MOUSE 2";
    if (vk == VK_XBUTTON1) return "M4";
    if (vk == VK_XBUTTON2) return "M5";
    char buf[64] = {};
    UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LONG lp = (sc << 16);
    if (GetKeyNameTextA(lp, buf, sizeof(buf)) > 0) return buf;
    return "KEY_" + std::to_string(vk);
}

} // namespace

KeybindSetting::KeybindSetting(Module* o, std::string n, int def, BindMode m)
    : Setting(o, std::move(n), SettingType::Keybind), vk_(def), mode_(m) {}

bool KeybindSetting::draw() {
    ImGui::Text("%s", name_.c_str());
    ImGui::SameLine();
    std::string label = capturing_ ? "[press key]" : vkName(vk_);
    label += "##bind_" + name_;
    bool changed = false;
    if (ImGui::Button(label.c_str())) { capturing_ = !capturing_; }
    if (capturing_) {
        for (int i = 1; i < 256; ++i) {
            if (GetAsyncKeyState(i) & 0x8000) {
                if (i == VK_ESCAPE) { vk_ = 0; }
                else { vk_ = i; }
                capturing_ = false;
                changed    = true;
                break;
            }
        }
    }
    return changed;
}

nlohmann::json KeybindSetting::save() const {
    return {{"vk", vk_}, {"mode", static_cast<int>(mode_)}};
}
void KeybindSetting::load(const nlohmann::json& j) {
    if (j.is_object()) {
        if (j.contains("vk") && j["vk"].is_number_integer()) setVK(j["vk"].get<int>());
        if (j.contains("mode") && j["mode"].is_number_integer()) {
            int mode = j["mode"].get<int>();
            mode_ = mode >= 0 && mode <= 3 ? static_cast<BindMode>(mode) : BindMode::Toggle;
        }
    }
}

} // namespace dt
