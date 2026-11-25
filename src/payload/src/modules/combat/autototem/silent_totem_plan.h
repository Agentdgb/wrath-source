#pragma once
namespace dt::modules::autototem {
struct SilentTotemPlan {
    int source = -1; // PlayerInventory index
    int button = 40; // SWAP target: offhand only
    int containerSlot() const { return source >= 0 && source < 9 ? source + 36 : source; }
};
inline SilentTotemPlan silentTotemPlan(bool offhandFull, int hotbarTotem, int inventoryTotem) {
    if (offhandFull) return {};
    return {hotbarTotem >= 0 ? hotbarTotem : inventoryTotem, 40};
}
}
