#include "core/module/ModuleManager.h"

#include "util/Log.h"
#include "util/XorStr.h"

#include <algorithm>
#include <vector>

namespace dt {

namespace {

std::vector<ModuleFactory>& factories() {
    static std::vector<ModuleFactory> f;
    return f;
}

} // namespace

ModuleManager& ModuleManager::get() {
    static ModuleManager g;
    return g;
}

void ModuleManager::registerFactory(ModuleFactory f) {
    factories().push_back(f);
}

void ModuleManager::initAll() {
    std::lock_guard state(clientStateMutex());
    EventBus::get().start();
    modules_.clear();
    modules_.reserve(factories().size());
    for (auto f : factories()) {
        auto m = f();
        m->onInit();
        modules_.push_back(std::move(m));
    }
    std::stable_sort(modules_.begin(), modules_.end(),
        [](auto& a, auto& b){ return a->name() < b->name(); });
    VLOG_INFO("mods", "Initialized {} modules", modules_.size());
}

void ModuleManager::shutdownAll() {
    std::lock_guard state(clientStateMutex());
    EventBus::get().stop();
    for (auto& m : modules_) if (m->isEnabled()) m->setEnabled(false);
    modules_.clear();
}

std::vector<Module*> ModuleManager::byCategory(Category c) const {
    std::vector<Module*> out;
    for (auto& m : modules_) if (m->category() == c) out.push_back(m.get());
    return out;
}

Module* ModuleManager::find(std::string_view name) const {
    for (auto& m : modules_) if (m->name() == name) return m.get();
    if (name == XOR_STR("Auto Crystal") || name == XOR_STR("Auto")) {
        for (auto& m : modules_) if (m->name() == "Fast Crystal") return m.get();
    }
    if (name == XOR_STR("AutoTotem")) {
        for (auto& m : modules_) if (m->name() == "Totem Assist") return m.get();
    }
    if (name == XOR_STR("SilentAim") || name == XOR_STR("Silent Aim")) {
        for (auto& m : modules_) if (m->name() == "Rotation Assist") return m.get();
    }
    if (name == XOR_STR("BlockESP")) {
        for (auto& m : modules_) if (m->name() == "Block Outline") return m.get();
    }
    if (name == XOR_STR("NoJumpDelay")) {
        for (auto& m : modules_) if (m->name() == "Jump Assist") return m.get();
    }
    if (name == XOR_STR("Target HUD")) {
        for (auto& m : modules_) if (m->name() == "Target Display") return m.get();
    }
    if (name == XOR_STR("TriggerBot") || name == XOR_STR("Triggerbot")) {
        for (auto& m : modules_) if (m->name() == "Hit Assist") return m.get();
    }
    return nullptr;
}

nlohmann::json ModuleManager::saveAll() const {
    std::lock_guard state(clientStateMutex());
    nlohmann::json j = nlohmann::json::object();
    for (auto& m : modules_) {
        auto& slot = j[std::string(m->name())];
        const auto mode = m->keybind().mode();
        slot["enabled"] = m->isEnabled() && mode != BindMode::Once && mode != BindMode::Hold;
        auto& settings = slot["settings"] = nlohmann::json::object();
        for (auto* s : m->settings()) {
            settings[std::string(s->name())] = s->save();
        }
    }
    return j;
}

void ModuleManager::loadAll(const nlohmann::json& j) {
    std::lock_guard state(clientStateMutex());
    if (!j.is_object()) return;
    for (auto& m : modules_) {
        auto it = j.find(std::string(m->name()));
        if (it == j.end()) {
            if (m->name() == "Fast Crystal") {
                it = j.find(XOR_STR("Auto Crystal"));
                if (it == j.end()) it = j.find(XOR_STR("Auto"));
            } else if (m->name() == "Totem Assist") {
                it = j.find(XOR_STR("AutoTotem"));
            } else if (m->name() == "Rotation Assist") {
                it = j.find(XOR_STR("SilentAim"));
                if (it == j.end()) it = j.find(XOR_STR("Silent Aim"));
            } else if (m->name() == "Block Outline") {
                it = j.find(XOR_STR("BlockESP"));
            } else if (m->name() == "Jump Assist") {
                it = j.find(XOR_STR("NoJumpDelay"));
            } else if (m->name() == "Target Display") {
                it = j.find(XOR_STR("Target HUD"));
            } else if (m->name() == "Hit Assist") {
                it = j.find(XOR_STR("TriggerBot"));
                if (it == j.end()) it = j.find(XOR_STR("Triggerbot"));
            }
        }
        if (it == j.end()) continue;
        if (it->contains("settings") && it->at("settings").is_object()) {
            const auto& settings_obj = it->at("settings");
            for (auto* s : m->settings()) {
                auto sit = settings_obj.find(std::string(s->name()));
                if (sit == settings_obj.end()) {
                    if (s->name() == "Glowstone Fast") sit = settings_obj.find(XOR_STR("Double Glowstone"));
                    else if (s->name() == "Action Delay MS") sit = settings_obj.find(XOR_STR("Switch Delay MS"));
                    else if (s->name() == "Slot Return") sit = settings_obj.find(XOR_STR("Switch Back"));
                    else if (s->name() == "Quick Swap") sit = settings_obj.find(XOR_STR("Silent Swap"));
                    else if (s->name() == "Rotation Assist") sit = settings_obj.find(XOR_STR("Silent Aim"));
                }
                // Preserve AntiBot profiles saved before the UI switched to seconds.
                if (sit == settings_obj.end() && m->name() == "AntiBot" && s->name() == "Inactividad (segundos)") {
                    const auto legacy = settings_obj.find("Idle delay (ms)");
                    if (legacy != settings_obj.end() && legacy->is_number())
                        s->load(legacy->get<double>() / 1000.0);
                }
                if (sit == settings_obj.end() && (m->name() == XOR_STR("Hit Assist") || m->name() == XOR_STR("TriggerBot")) && s->name() == XOR_STR("WaitForCrit")) {
                    const auto recent = settings_obj.find("Attack mode");
                    if (recent != settings_obj.end() && recent->is_string())
                        s->load(recent->get<std::string>() == "Prioritize criticals");
                }
                if (sit != settings_obj.end()) {
                    if (m->name() == XOR_STR("AimAssist") && s->name() == "Focus mode" && sit->is_string())
                        s->load(sit->get<std::string>() != "Off");
                    else
                        s->load(*sit);
                }
            }
        }
        const auto mode = m->keybind().mode();
        m->setEnabled(it->value("enabled", false) && mode != BindMode::Once && mode != BindMode::Hold);
    }
}

void ModuleManager::panic() {
    std::lock_guard state(clientStateMutex());
    panicking_ = true;
    panicked_ = true;
    int off = 0;
    for (auto& m : modules_) if (m->isEnabled()) { m->setEnabled(false); ++off; }
    panicking_ = false;
    VLOG_WARN("mods", "PANIC — disabled {} modules", off);
}

} // namespace dt
