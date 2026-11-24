#pragma once

namespace vanta {

struct Event {};

struct Cancellable {
    bool cancelled = false;
    void cancel() noexcept { cancelled = true; }
};

} // namespace vanta
