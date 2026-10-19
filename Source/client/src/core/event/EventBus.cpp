#include "core/event/EventBus.h"

namespace vanta {

EventBus& EventBus::get() {
    static EventBus g;
    return g;
}

} // namespace vanta
