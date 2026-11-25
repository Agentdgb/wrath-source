#pragma once
#include "hook/HookId.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace dt::hook {

enum class Recipe : std::uint8_t {
    A_PreCancel,
    B_ReturnRewrite,
};

// One entry per hook site. Multiple candidates per method to survive rename
// drift across 1.21.x point releases; the loader picks the first that resolves.
struct MethodCandidate {
    std::string_view canonName;   // "onTick", "attackTargetEntity", etc.
    std::string_view canonDesc;   // "()V" — descriptor uses canonical L...; names
};

struct HookRow {
    HookId                        id;
    std::string_view              ownerCanon;
    std::vector<MethodCandidate>  candidates;
    Recipe                        recipe;
    HookLoad                      load;
    // For Recipe A: the return opcode to emit on cancel path (RETURN/IRETURN/FRETURN/ARETURN).
    std::uint8_t                  returnOpcode;
    // Bridge trampoline name + descriptor (JVMTI-visible).
    std::string_view              bridgeMethod;
    std::string_view              bridgeDesc;
};

class HookRegistry {
public:
    static HookRegistry& get();
    void addRow(HookRow row);
    const std::vector<HookRow>& rows() const noexcept { return rows_; }
private:
    std::vector<HookRow> rows_;
};

#define DT_HR_CAT2_(a, b) a##b
#define DT_HR_CAT_(a, b)  DT_HR_CAT2_(a, b)
#define DT_REGISTER_HOOK_SITE(fn)                                                       \
    namespace {                                                                            \
        static const int DT_HR_CAT_(_dt_reg_site_, __COUNTER__) = [](){              \
            ::dt::hook::HookRegistry::get().addRow(fn());                               \
            return 0;                                                                      \
        }();                                                                               \
    }

} // namespace dt::hook
