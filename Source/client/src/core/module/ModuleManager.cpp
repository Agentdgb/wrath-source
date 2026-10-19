#include "core/module/ModuleManager.h"

#include "util/Log.h"

#include <algorithm>
#include <vector>

namespace vanta {

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
        if (it == j.end() && m->name() == "Auto Crystal") it = j.find("Auto");
        if (it == j.end()) continue;
        if (it->contains("settings") && it->at("settings").is_object()) {
            for (auto* s : m->settings()) {
                auto sit = it->at("settings").find(std::string(s->name()));
                if (sit != it->at("settings").end()) s->load(*sit);
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

} // namespace vanta
