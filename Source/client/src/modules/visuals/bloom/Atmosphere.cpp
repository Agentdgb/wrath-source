#include "Atmosphere.h"
#include "AtmosphereMath.h"
#include "SkyRenderer.h"
#include "core/NativeCallbacks.h"
#include "modules/visuals/ports/PortJni.h"
#include "vgui/bridge.h"
#include <array>
#include <chrono>
#include <jnihook.h>
#include <string>
namespace vanta::modules::bloom {
AtmosphereSettings atmosphere;
namespace {
namespace jni = visualports::jni;
AtmosphereModel model;
float frameDelta = 0, naturalLight = 1;
bool tried = false;
std::string status = "Native sky, stars and fog";
jclass owner = nullptr, fogOwner = nullptr, skyOwner = nullptr, integerClass = nullptr, floatClass = nullptr,
       vectorClass = nullptr;
jmethodID original = nullptr, fogOriginal = nullptr, sunOriginal = nullptr, moonOriginal = nullptr, intValue = nullptr,
          floatValue = nullptr, boxInt = nullptr, boxFloat = nullptr, vectorCtor = nullptr, onThread = nullptr;
jfieldID vx = nullptr, vy = nullptr, vz = nullptr, vw = nullptr;
jobject clientRef = nullptr, lastWorld = nullptr, moonKey = nullptr, fullMoon = nullptr, crescent = nullptr;
std::array<jobject, 14> keys{};
constexpr const char *fieldNames[] = {"field_63746", "field_63742", "field_64126", "field_64339", "field_64345",
                                      "field_64120", "field_64121", "field_64122", "field_64123", "field_64344",
                                      "field_64340", "field_64341", "field_64342", "field_64346"};
jobject JNICALL attributes(JNIEnv *e, jobject self, jobject attribute, jfloat partial) {
    NativeCallback callback;
    auto value = e->CallNonvirtualObjectMethod(self, owner, original, attribute, partial);
    if (e->ExceptionCheck() || !value || !clientRef)
        return value;
    if (!e->CallBooleanMethod(clientRef, onThread) || e->ExceptionCheck())
        return value;
    if (model.fade <= .001f && model.weather.cv <= .001f && model.timeFade <= .0001f)
        return value;
    if (e->PushLocalFrame(16) < 0)
        return value;
    try {
        int index = -1;
        for (int i = 0; i < int(keys.size()); ++i)
            if (e->IsSameObject(keys[i], attribute)) {
                index = i;
                break;
            }
        if (index < 0) {
            if (e->IsSameObject(attribute, moonKey) && model.weather.cv > .5f) {
                auto &pw = model.weather.pw;
                if (pw[8] > .5f)
                    return e->PopLocalFrame(e->NewLocalRef(fullMoon));
                if (pw[4] > .5f)
                    return e->PopLocalFrame(e->NewLocalRef(crescent));
            }
        } else {
            auto key = static_cast<SkyAttribute>(index);
            if (e->IsInstanceOf(value, integerClass)) {
                auto old = static_cast<std::uint32_t>(e->CallIntMethod(value, intValue));
                if (key == SkyAttribute::Sky)
                    model.weather.eu =
                        std::max({float(old >> 16 & 255), float(old >> 8 & 255), float(old & 255)}) / 255;
                auto changed = model.color(atmosphere, key, old);
                if (changed != old) {
                    auto result = e->CallStaticObjectMethod(integerClass, boxInt, static_cast<jint>(changed));
                    jni::check(e);
                    return e->PopLocalFrame(result);
                }
            } else if (e->IsInstanceOf(value, floatClass)) {
                float old = e->CallFloatMethod(value, floatValue);
                auto changed = model.number(atmosphere, key, old);
                if (key == SkyAttribute::SunAngle)
                    model.weather.er = changed;
                if (key == SkyAttribute::MoonAngle)
                    model.weather.es = changed;
                if (key == SkyAttribute::Stars) {
                    auto &v = model.weather;
                    v.eq = std::max(changed, (v.pw[3] + v.pw[4] + .6f * v.pw[10] + v.dj) * v.cv * v.da);
                }
                if (std::isfinite(changed) && changed != old) {
                    auto result = e->CallStaticObjectMethod(floatClass, boxFloat, changed);
                    jni::check(e);
                    return e->PopLocalFrame(result);
                }
            }
        }
    } catch (...) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
    }
    e->PopLocalFrame(nullptr);
    return value;
}
jobject JNICALL fogColor(JNIEnv *e, jobject self, jobject camera, jfloat tick, jobject world, jint distance,
                         jfloat darkness) {
    NativeCallback callback;
    auto value = e->CallNonvirtualObjectMethod(self, fogOwner, fogOriginal, camera, tick, world, distance, darkness);
    if (e->ExceptionCheck() || !value || model.fade <= .001f || model.submerged)
        return value;
    float opacity = model.fade * (1 - std::min(1.f, model.weather.cv * model.weather.da * 1.45f));
    float c[3];
    auto fields = std::array<jfieldID, 3>{vx, vy, vz};
    for (int k = 0; k < 3; ++k) {
        float old = e->GetFloatField(value, fields[k]),
              target = atmosphere.mode == 1 ? model.base[k] : old * model.base[k];
        c[k] = old + (target - old) * opacity;
    }
    return e->NewObject(vectorClass, vectorCtor, c[0], c[1], c[2], e->GetFloatField(value, vw));
}
void JNICALL sun(JNIEnv *e, jobject self, jfloat opacity, jobject matrices) {
    NativeCallback callback;
    naturalLight = std::clamp(opacity, 0.f, 1.f);
    auto &v = model.weather;
    float coverage = 0;
    if (v.cv > .001f && model.overworld) {
        coverage = v.cv * v.da * (v.pw[7] * .85f + sky::sunCoverage() * (v.pw[11] + v.pw[1] + v.pw[7]) * 2);
    }
    e->CallNonvirtualVoidMethod(self, skyOwner, sunOriginal, opacity * (1 - std::clamp(coverage, 0.f, 1.f)), matrices);
}
void JNICALL moon(JNIEnv *e, jobject self, jobject phase, jfloat opacity, jobject matrices) {
    NativeCallback callback;
    float coverage = model.overworld ? sky::moonCoverage() : 0;
    e->CallNonvirtualVoidMethod(self, skyOwner, moonOriginal, phase, opacity * (1 - std::clamp(coverage, 0.f, 1.f)),
                                matrices);
}
jclass global(JNIEnv *e, const char *name) {
    auto c = jni::cls(e, name);
    auto g = static_cast<jclass>(e->NewGlobalRef(c));
    jni::check(e);
    if (!g)
        throw std::runtime_error("Atmosphere class reference unavailable");
    return g;
}
void install(JNIEnv *e) {
    owner = global(e, "net/minecraft/class_12202");
    fogOwner = global(e, "net/minecraft/class_758");
    skyOwner = global(e, "net/minecraft/class_9975");
    integerClass = global(e, "java/lang/Integer");
    floatClass = global(e, "java/lang/Float");
    vectorClass = global(e, "org/joml/Vector4f");
    intValue = jni::method(e, integerClass, "intValue", "()I");
    floatValue = jni::method(e, floatClass, "floatValue", "()F");
    boxInt = jni::smethod(e, integerClass, "valueOf", "(I)Ljava/lang/Integer;");
    boxFloat = jni::smethod(e, floatClass, "valueOf", "(F)Ljava/lang/Float;");
    vectorCtor = jni::method(e, vectorClass, "<init>", "(FFFF)V");
    vx = jni::field(e, vectorClass, "x", "F");
    vy = jni::field(e, vectorClass, "y", "F");
    vz = jni::field(e, vectorClass, "z", "F");
    vw = jni::field(e, vectorClass, "w", "F");
    auto client = jni::mc(e);
    clientRef = e->NewGlobalRef(client);
    jni::check(e);
    onThread = jni::method(e, jni::cls(e, "net/minecraft/class_310"), "method_18854", "()Z");
    auto attributesClass = jni::cls(e, "net/minecraft/class_12206");
    auto key = [&](const char *name) {
        auto f = e->GetStaticFieldID(attributesClass, name, "Lnet/minecraft/class_12197;");
        jni::check(e);
        auto result = e->NewGlobalRef(e->GetStaticObjectField(attributesClass, f));
        jni::check(e);
        return result;
    };
    for (int i = 0; i < int(keys.size()); ++i)
        keys[i] = key(fieldNames[i]);
    moonKey = key("field_64343");
    auto moonClass = jni::cls(e, "net/minecraft/class_12131");
    auto phase = [&](const char *name) {
        auto f = e->GetStaticFieldID(moonClass, name, "Lnet/minecraft/class_12131;");
        jni::check(e);
        auto result = e->NewGlobalRef(e->GetStaticObjectField(moonClass, f));
        jni::check(e);
        return result;
    };
    fullMoon = phase("field_63425");
    crescent = phase("field_63430");
    auto attach = [&](jclass c, const char *name, const char *descriptor, void *replacement, jmethodID *trampoline) {
        auto method = jni::method(e, c, name, descriptor);
        if (JNIHook_Attach(method, replacement, trampoline) != JNIHOOK_OK)
            throw std::runtime_error(std::string("Atmosphere hook unavailable: ") + name);
    };
    attach(owner, "method_75689", "(Lnet/minecraft/class_12197;F)Ljava/lang/Object;",
           reinterpret_cast<void *>(&attributes), &original);
    attach(fogOwner, "method_62185", "(Lnet/minecraft/class_4184;FLnet/minecraft/class_638;IF)Lorg/joml/Vector4f;",
           reinterpret_cast<void *>(&fogColor), &fogOriginal);
    attach(skyOwner, "method_62303", "(FLnet/minecraft/class_4587;)V", reinterpret_cast<void *>(&sun), &sunOriginal);
    attach(skyOwner, "method_62304", "(Lnet/minecraft/class_12131;FLnet/minecraft/class_4587;)V",
           reinterpret_cast<void *>(&moon), &moonOriginal);
    status = "Native sky, stars and fog";
}
void context(JNIEnv *e) {
    auto client = jni::mc(e), world = jni::world(e, client);
    if (!e->IsSameObject(world, lastWorld)) {
        if (lastWorld)
            e->DeleteGlobalRef(lastWorld);
        lastWorld = world ? e->NewGlobalRef(world) : nullptr;
        model = AtmosphereModel{};
        naturalLight = 1;
        sky::reset();
    }
    model.overworld = false;
    model.submerged = false;
    if (!world)
        return;
    auto wc = jni::cls(e, "net/minecraft/class_1937");
    auto dimension = e->CallObjectMethod(world, jni::method(e, wc, "method_8597", "()Lnet/minecraft/class_2874;"));
    auto skybox = e->CallObjectMethod(dimension, jni::method(e, jni::cls(e, "net/minecraft/class_2874"), "comp_5220",
                                                             "()Lnet/minecraft/class_2874$class_12326;"));
    auto type = jni::cls(e, "net/minecraft/class_2874$class_12326");
    auto overworld = e->GetStaticObjectField(
        type, e->GetStaticFieldID(type, "field_64386", "Lnet/minecraft/class_2874$class_12326;"));
    jni::check(e);
    model.overworld = e->IsSameObject(skybox, overworld);
    auto renderer = e->GetObjectField(
        client, jni::field(e, jni::cls(e, "net/minecraft/class_310"), "field_1773", "Lnet/minecraft/class_757;"));
    auto camera = e->CallObjectMethod(renderer, jni::method(e, jni::cls(e, "net/minecraft/class_757"), "method_19418",
                                                            "()Lnet/minecraft/class_4184;"));
    auto sub = e->CallObjectMethod(camera, jni::method(e, jni::cls(e, "net/minecraft/class_4184"), "method_19334",
                                                       "()Lnet/minecraft/class_5636;"));
    auto ordinal = e->CallIntMethod(sub, jni::method(e, jni::cls(e, "java/lang/Enum"), "ordinal", "()I"));
    model.submerged = ordinal >= 0 && ordinal <= 2;
    model.weather.dk = 1 - e->CallFloatMethod(world, jni::method(e, wc, "method_8430", "(F)F"), 1.f);
    jni::check(e);
}
} // namespace
AtmosphereModel &atmosphereModel() { return model; }
float atmosphereFrameDelta() { return frameDelta; }
float atmosphereNaturalLight() { return naturalLight; }
bool atmosphereSkyActive() {
    return (atmosphere.enabled && atmosphere.sky && atmosphere.preset < 13) || model.weather.cv > .001f ||
           model.fade * model.tint > .001f;
}
void tickAtmosphere(float ms) {
    frameDelta = std::clamp(ms / 1000, 0.f, .1f);
    float r, g, b, a;
    vgui::bridge::clientAccent(r, g, b, a);
    auto clock = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (atmosphere.enabled || model.fade > .001f || model.weather.cv > .001f) {
        jvm::JniScope scope;
        if (scope) {
            auto *e = scope.get();
            if (e->PushLocalFrame(128) >= 0) {
                try {
                    context(e);
                    if (atmosphere.enabled && !tried) {
                        tried = true;
                        auto init = JNIHook_Init(jvm::ctx().vm.load());
                        if (init != JNIHOOK_OK)
                            throw std::runtime_error("Atmosphere render hooks unavailable");
                        install(e);
                    }
                } catch (const std::exception &ex) {
                    if (e->ExceptionCheck())
                        e->ExceptionClear();
                    status = ex.what();
                }
                e->PopLocalFrame(nullptr);
            }
        }
    }
    model.update(atmosphere, frameDelta, clock, {r, g, b});
}
const char *atmosphereStatus() {
    if (auto error = atmosphereSkyError())
        return error;
    return status.c_str();
}
void stopAtmosphere() {
    atmosphere.enabled = false;
    model = AtmosphereModel{};
    sky::reset();
}
void freeAtmosphere(JNIEnv *e) {
    for (auto &key : keys) {
        if (key)
            e->DeleteGlobalRef(key);
        key = nullptr;
    }
    for (auto *o : {&moonKey, &fullMoon, &crescent, &clientRef, &lastWorld}) {
        if (*o)
            e->DeleteGlobalRef(*o);
        *o = nullptr;
    }
    for (auto *c : {&owner, &fogOwner, &skyOwner, &integerClass, &floatClass, &vectorClass}) {
        if (*c)
            e->DeleteGlobalRef(*c);
        *c = nullptr;
    }
}
} // namespace vanta::modules::bloom
