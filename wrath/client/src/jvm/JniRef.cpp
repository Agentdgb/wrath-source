#include "jvm/JniRef.h"

namespace vanta::jvm {

std::string toStdString(JNIEnv* env, jstring js) {
    if (!env || !js) return {};
    const char* p = env->GetStringUTFChars(js, nullptr);
    if (!p) return {};
    std::string out(p);
    env->ReleaseStringUTFChars(js, p);
    return out;
}

} // namespace vanta::jvm
