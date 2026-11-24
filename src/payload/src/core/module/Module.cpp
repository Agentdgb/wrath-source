#include "core/module/Module.h"

#include "core/config/ConfigManager.h"
#include "core/module/ModuleManager.h"
#include "util/Log.h"
#include "util/StringScrub.h"

#include <imgui.h>

namespace vanta {

Module::Module(std::string name, std::string desc, Category cat, int defaultKey)
    : name_(std::move(name)), desc_(std::move(desc)), category_(cat),
      keybind_(this, "Bind", defaultKey, BindMode::Toggle) {
    registerSetting(&keybind_);
}

Module::~Module() {
    listeners_.clear();
    vanta::util::scrubString(name_);
    vanta::util::scrubString(desc_);
}

void Module::registerSetting(Setting* s) {
    settings_.push_back(s);
}

void Module::toggle() { std::lock_guard state(clientStateMutex()); setEnabled(!enabled_); }

void Module::setEnabled(bool e) {
    std::lock_guard state(clientStateMutex());
    if (e == enabled_) return;
    enabled_ = e;

    // Logged because "the module does not work" and "the module was never
    // switched on" look identical from outside, and the toggle happens in the
    // gui where nothing else records it.
    VLOG_INFO("mods", "{} {}", name_, e ? "enabled" : "disabled");

    if (e) onEnable(); else onDisable();

    events::ModuleToggleEvent evt{{}, name_.c_str(), enabled_};
    EventBus::get().post(evt);

    if (keybind_.mode() != BindMode::Once && keybind_.mode() != BindMode::Hold
        && !ConfigManager::get().isLoading() && !ModuleManager::get().isPanicking()) {
        ConfigManager::get().markDirty();
    }
}

void Module::drawImGui() {
    std::lock_guard state(clientStateMutex());
    bool anyChanged = false;
    for (Setting* s : settings_) {
        if (!s->visible()) continue;
        if (s->draw()) anyChanged = true;
    }
    if (anyChanged && !ConfigManager::get().isLoading()) {
        ConfigManager::get().markDirty();
    }
    if (!desc_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 180, 180, 200));
        ImGui::TextWrapped("%s", desc_.c_str());
        ImGui::PopStyleColor();
    }
}

void _register_module_factory(ModuleFactory f) {
    ModuleManager::registerFactory(f);
}

} // namespace vanta
