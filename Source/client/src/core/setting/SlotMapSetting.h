#pragma once

#include "core/setting/Setting.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vanta {

// One assignment per hotbar slot — the storage behind the nine-square strip the
// vgui draws at the bottom of a drawer.
//
// Each slot holds an index into a caller-supplied option list, or EMPTY. The
// list of options and, separately, WHICH of them are currently selectable are
// both owned by the module rather than by this class: a Refill module lets the
// player tick which item kinds it handles, and only those may then be assigned
// to a slot. Rather than have the widget reach into another module's settings,
// the module hands over a predicate and this stays a dumb, serialisable store.
//
// Invariant: a slot never holds an option that is not currently allowed. It is
// enforced on read, so unticking a kind clears the slots that used it without
// anything having to remember to go and do that.
class SlotMapSetting : public Setting {
public:
    static constexpr int SLOTS = 9;
    static constexpr int EMPTY = -1;

    SlotMapSetting(Module* owner, std::string name, std::vector<std::string> options);

    // Raw stored value, before the allowed filter. Config code wants this.
    int  rawSlot(int index) const noexcept;

    // What the slot actually resolves to right now: EMPTY if the option it
    // holds is no longer selectable.
    int  slot(int index) const noexcept;

    void setSlot(int index, int option) noexcept;

    // Advances a slot to the next ALLOWED option, wrapping through EMPTY. This
    // is what a click on a square does: with a handful of options a cycle is
    // less machinery than a popup and cannot land on a disallowed value.
    void cycleSlot(int index) noexcept;

    const std::vector<std::string>& options() const noexcept { return options_; }

    // Bit i set = option i may be assigned. Defaults to "all allowed" when the
    // owner never supplies a predicate.
    void     setAllowedProvider(std::function<std::uint32_t()> p) { allowed_ = std::move(p); }
    std::uint32_t allowedMask() const;
    bool     isAllowed(int option) const;

    bool           draw() override;
    nlohmann::json save() const override;
    void           load(const nlohmann::json& j) override;

private:
    std::array<int, SLOTS>          slots_{};
    std::vector<std::string>        options_;
    std::function<std::uint32_t()>  allowed_{};
};

} // namespace vanta
