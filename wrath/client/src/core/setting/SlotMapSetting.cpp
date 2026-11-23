#include "core/setting/SlotMapSetting.h"

#include <imgui.h>

namespace vanta {

SlotMapSetting::SlotMapSetting(Module* o, std::string n, std::vector<std::string> options)
    : Setting(o, std::move(n), SettingType::SlotMap),
      options_(std::move(options))
{
    slots_.fill(EMPTY);
}

std::uint32_t SlotMapSetting::allowedMask() const
{
    if (allowed_) return allowed_();
    // No predicate supplied: every option is selectable.
    return options_.empty() ? 0u
         : (options_.size() >= 32 ? ~0u : ((1u << options_.size()) - 1u));
}

bool SlotMapSetting::isAllowed(int option) const
{
    if (option < 0 || option >= static_cast<int>(options_.size())) return false;
    return (allowedMask() >> option) & 1u;
}

int SlotMapSetting::rawSlot(int index) const noexcept
{
    if (index < 0 || index >= SLOTS) return EMPTY;
    return slots_[static_cast<std::size_t>(index)];
}

int SlotMapSetting::slot(int index) const noexcept
{
    const int raw = rawSlot(index);
    // Filtered on READ rather than cleared on change. Unticking a kind then
    // needs no bookkeeping anywhere, and — the part that actually matters —
    // re-ticking it brings the old assignment back instead of having silently
    // destroyed it.
    return isAllowed(raw) ? raw : EMPTY;
}

void SlotMapSetting::setSlot(int index, int option) noexcept
{
    if (index < 0 || index >= SLOTS) return;
    if (option != EMPTY && !isAllowed(option)) return;
    slots_[static_cast<std::size_t>(index)] = option;
}

void SlotMapSetting::cycleSlot(int index) noexcept
{
    if (index < 0 || index >= SLOTS) return;

    const int n = static_cast<int>(options_.size());
    int cur = slot(index);   // filtered, so a stale value restarts the cycle

    // Walk forward to the next allowed option; fall out to EMPTY after the
    // last one. Bounded by n + 1 steps, so an empty allowed-set simply lands
    // on EMPTY instead of spinning.
    for (int step = 0; step < n + 1; ++step) {
        cur = (cur + 1 >= n) ? EMPTY : cur + 1;
        if (cur == EMPTY || isAllowed(cur)) break;
    }
    slots_[static_cast<std::size_t>(index)] = cur;
}

bool SlotMapSetting::draw()
{
    // ImGui fallback only — the real strip lives in the vgui as
    // c_slot_map_setting. Kept so any leftover ImGui-driven flow still renders
    // something rather than nothing.
    bool changed = false;
    ImGui::TextUnformatted(name_.c_str());
    for (int i = 0; i < SLOTS; ++i) {
        ImGui::PushID(i);
        const int v = slot(i);
        const char* label = (v == EMPTY) ? "-" : options_[static_cast<std::size_t>(v)].c_str();
        if (ImGui::SmallButton(label)) { cycleSlot(i); changed = true; }
        ImGui::PopID();
        if (i != SLOTS - 1) ImGui::SameLine();
    }
    return changed;
}

nlohmann::json SlotMapSetting::save() const
{
    // Raw values, not filtered ones: a kind that happens to be unticked when
    // the config is written must not lose its slot assignments.
    nlohmann::json arr = nlohmann::json::array();
    for (int v : slots_) arr.push_back(v);
    return arr;
}

void SlotMapSetting::load(const nlohmann::json& j)
{
    if (!j.is_array()) return;
    const int n = static_cast<int>(options_.size());
    for (int i = 0; i < SLOTS && i < static_cast<int>(j.size()); ++i) {
        int v = j[static_cast<std::size_t>(i)].get<int>();
        if (v < 0 || v >= n) v = EMPTY;
        slots_[static_cast<std::size_t>(i)] = v;
    }
}

} // namespace vanta
