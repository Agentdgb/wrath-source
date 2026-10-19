#include "jvm/HandleTable.h"

#include <vector>

namespace vanta::jvm {

struct HandleTable::Impl {
    std::vector<jobject> slots;      // slot 0 reserved (0 == invalid)
    std::vector<std::uint32_t> free; // stack of free slot indices
    Impl() { slots.emplace_back(nullptr); }
};

HandleTable::HandleTable()  : impl_(new Impl()) {}
HandleTable::~HandleTable() { delete impl_; }

std::uint64_t HandleTable::acquire(JNIEnv* env, jobject o) {
    if (!env || !o) return kInvalid;
    jobject g = env->NewGlobalRef(o);
    if (!g) return kInvalid;

    std::uint32_t idx;
    if (!impl_->free.empty()) {
        idx = impl_->free.back();
        impl_->free.pop_back();
        impl_->slots[idx] = g;
    } else {
        idx = static_cast<std::uint32_t>(impl_->slots.size());
        impl_->slots.push_back(g);
    }
    return static_cast<std::uint64_t>(idx);
}

jobject HandleTable::resolve(std::uint64_t h) const noexcept {
    if (h == kInvalid || h >= impl_->slots.size()) return nullptr;
    return impl_->slots[static_cast<std::size_t>(h)];
}

void HandleTable::releaseAll(JNIEnv* env) {
    if (!env) return;
    for (std::size_t i = 1; i < impl_->slots.size(); ++i) {
        if (impl_->slots[i]) env->DeleteGlobalRef(impl_->slots[i]);
        impl_->slots[i] = nullptr;
    }
    impl_->free.clear();
    // slot 0 stays reserved
}

void HandleTable::tickReset(JNIEnv* env) {
    // For v0.1 we release everything each tick. A tenure system can be added
    // later if a bridge grows a use case for cross-tick handles.
    releaseAll(env);
}

HandleTable& threadHandles() {
    thread_local HandleTable t;
    return t;
}

} // namespace vanta::jvm
