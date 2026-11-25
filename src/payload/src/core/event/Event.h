#pragma once

namespace dt {

struct Event {};

struct Cancellable {
    bool cancelled = false;
    void cancel() noexcept { cancelled = true; }
};

} // namespace dt
