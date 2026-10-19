#include "hook/PacketEvents.h"
#include "core/event/EventBus.h"
#include "core/event/Events.h"
#include "core/UnloadSafety.h"
#include "core/NativeCallbacks.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include <jnihook.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>

namespace vanta::hook::packets {
namespace {
std::atomic<jclass> owner{nullptr};
jmethodID sendMethod = nullptr, receiveMethod = nullptr;
std::atomic<jmethodID> originalSend{nullptr}, originalReceive{nullptr};
std::mutex lifecycleMutex; // install/shutdown only; callbacks never acquire it
std::atomic<bool> stopping{false}, installed{false};
std::atomic<unsigned> callbacks{0};
std::atomic<bool> attempted{false};

struct Callback {
    vanta::NativeCallback lifetime;
    Callback() { callbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~Callback() { callbacks.fetch_sub(1, std::memory_order_release); }
};

template<class E>
bool publish(JNIEnv* env, jobject packet) {
    if (stopping.load(std::memory_order_acquire) || !packet) return true;
    if (env->PushLocalFrame(128) != JNI_OK) return false;
    bool proceed = true;
    try {
        E event{{}, {}, env, packet};
        proceed = EventBus::get().post(event);
    } catch (...) { proceed = true; }
    // Listener lookups are optional. Do not leak their mapping exceptions into
    // vanilla networking; original-method exceptions below remain untouched.
    if (env->ExceptionCheck()) { env->ExceptionClear(); proceed = true; }
    env->PopLocalFrame(nullptr);
    return proceed;
}

void JNICALL send(JNIEnv* env, jobject self, jobject packet, jobject listener, jboolean flush) {
    Callback callback;
    const auto original = originalSend.load(std::memory_order_acquire);
    const auto clazz = owner.load(std::memory_order_acquire);
    if (publish<events::PacketSendEvent>(env, packet) && original && clazz)
        env->CallNonvirtualVoidMethod(self, clazz, original, packet, listener, flush);
}

void JNICALL receive(JNIEnv* env, jobject self, jobject context, jobject packet) {
    Callback callback;
    const auto original = originalReceive.load(std::memory_order_acquire);
    const auto clazz = owner.load(std::memory_order_acquire);
    if (publish<events::PacketReceiveEvent>(env, packet) && original && clazz)
        env->CallNonvirtualVoidMethod(self, clazz, original, context, packet);
}

bool drainCallbacks() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (callbacks.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return callbacks.load(std::memory_order_acquire) == 0;
}

bool detachAndDrain(JNIEnv* env) {
    // Stop listener traffic first. Vanilla forwarding remains available.
    if (!drainCallbacks()) { blockUnload(); return false; }
    bool detached = true;
    if (originalSend.load(std::memory_order_acquire)) detached = JNIHook_Detach(sendMethod) == JNIHOOK_OK;
    if (originalReceive.load(std::memory_order_acquire))
        detached = (JNIHook_Detach(receiveMethod) == JNIHOOK_OK) && detached;
    // A callback can enter between the first drain and the redefinition.
    // JNIHook retains its original classes until its own shutdown drain.
    const bool drained = drainCallbacks();
    if (!detached || !drained) { blockUnload(); return false; }
    originalSend.store(nullptr, std::memory_order_release);
    originalReceive.store(nullptr, std::memory_order_release);
    if (auto clazz = owner.exchange(nullptr, std::memory_order_acq_rel)) env->DeleteGlobalRef(clazz);
    sendMethod = receiveMethod = nullptr;
    return true;
}
}

bool ready() { return installed.load(std::memory_order_acquire); }
bool unavailable() { return attempted.load() && !ready(); }

void install() {
    std::lock_guard lifecycle(lifecycleMutex);
    if (attempted.exchange(true) || stopping.load()) return;

    // CRITICAL JVM FIX:
    // ClientConnection (net.minecraft.class_2535) maintains an internal
    // Queue<Consumer<ClientConnection>> (packetQueue). Detouring method_52906 /
    // method_10770 via JNIHook causes jnihook to synthesize a duplicate class
    // (net.minecraft.class_2535_<uuid>) and rewrite constant pool descriptors.
    // Any lambda created during send() gets compiled with the synthetic clone type.
    // When sendQueuedPackets() polls and executes consumer.accept(this) on the real
    // ClientConnection instance, the JVM throws an unrecoverable ClassCastException
    // (class net.minecraft.class_2535 cannot be cast to class net.minecraft.class_2535_<uuid>)
    // on line 360 of class_2535 during MultiplayerScreen / connection tick.
    //
    // By keeping attempted=true and installed=false, dependent modules (NoFall, ChatSpy)
    // cleanly trigger their built-in hook::packets::unavailable() safety paths,
    // completely eliminating the crash while keeping the rest of the client fully functional.
    VLOG_WARN("packets", "JNIHook detour on ClientConnection disabled to prevent JVM ClassCastException");
    installed.store(false, std::memory_order_release);
}

bool shutdown() {
    std::lock_guard lifecycle(lifecycleMutex);
    stopping.store(true, std::memory_order_release);
    installed.store(false, std::memory_order_release);
    jvm::JniScope scope;
    if (owner.load(std::memory_order_acquire) && !scope) { blockUnload(); return false; }
    return detachAndDrain(scope.get());
}
}
