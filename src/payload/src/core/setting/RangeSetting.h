#pragma once

#include "core/setting/Setting.h"

namespace dt {

// A pair of ints sharing one absolute range — the storage-side counterpart of
// the double-thumb slider in the vgui. Kept as ints because every current
// consumer measures in milliseconds; a float variant can be added later if
// something needs fractional units.
//
// Invariants: min_ <= max_, both always inside [absoluteMin_, absoluteMax_].
class RangeSetting : public Setting {
public:
    RangeSetting(Module* owner, std::string name,
                 int defaultMin, int defaultMax,
                 int absoluteMin, int absoluteMax);

    int  min() const noexcept { return min_; }
    int  max() const noexcept { return max_; }

    int  absoluteMin() const noexcept { return absoluteMin_; }
    int  absoluteMax() const noexcept { return absoluteMax_; }

    // Adopt a new (min, max). Values are clamped to the absolute range and,
    // if the caller reversed them, swapped. Silent — no throw.
    void set(int newMin, int newMax) noexcept;

    bool           draw() override;
    nlohmann::json save() const override;
    void           load(const nlohmann::json& j) override;

private:
    int min_, max_;
    int absoluteMin_, absoluteMax_;
};

} // namespace dt
