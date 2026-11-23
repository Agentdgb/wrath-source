#include "hook/bridge/AltSlot.h"

// Recipe B is not wired in v0.1 (see docs/hook_layer.md). This TU exists so
// downstream files that #include it link cleanly; the functions are no-ops
// until we ship the value-modifying recipe path.

namespace vanta::hook::bridge {

jobject AltSlot::threadLocalArray(JNIEnv*)      { return nullptr; }
void    AltSlot::setLong(JNIEnv*, jlong)        {}
jlong   AltSlot::getLong(JNIEnv*)               { return 0; }

} // namespace vanta::hook::bridge
