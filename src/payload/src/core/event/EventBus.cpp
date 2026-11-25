#include "core/event/EventBus.h"

namespace dt {

EventBus& EventBus::get() {
    static EventBus g;
    return g;
}

} // namespace dt
