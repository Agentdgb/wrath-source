#pragma once
#include <mutex>

namespace vanta {
// The render/UI path, event dispatch and configuration snapshots share this
// lock. Recursive because listeners can toggle modules and publish events.
inline std::recursive_mutex& clientStateMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}
}
