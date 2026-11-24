#include "hook/HookRow.h"

namespace vanta::hook {

HookRegistry& HookRegistry::get() {
    static HookRegistry g;
    return g;
}

void HookRegistry::addRow(HookRow r) { rows_.push_back(std::move(r)); }

} // namespace vanta::hook
