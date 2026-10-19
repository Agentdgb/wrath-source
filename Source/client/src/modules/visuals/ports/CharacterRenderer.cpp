#include "CharacterData.h"
#include "PortJni.h"
#include "VisualPorts.h"
#include "core/NativeCallbacks.h"
#include "core/UnloadSafety.h"
#include "core/social/EntityFriends.h"
#include "util/Log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <jnihook.h>
#include <string>
#include <unordered_map>
namespace vanta::modules::visualports {
namespace {
struct Cache {
    jclass layer = nullptr, draw = nullptr, label = nullptr, renderer = nullptr, stack = nullptr, part = nullptr,
           state = nullptr, vertex = nullptr, avatar = nullptr, playerModel = nullptr;
    jfieldID features = nullptr, model = nullptr, delegate = nullptr, owner = nullptr, id = nullptr, key = nullptr,
             light = nullptr, overlay = nullptr;
    jfieldID args[5]{}, bone[6]{}, px = nullptr, py = nullptr, pz = nullptr, pitch = nullptr, yaw = nullptr,
                                   roll = nullptr, sx = nullptr, sy = nullptr, sz = nullptr;
    jmethodID layerCtor = nullptr, drawCtor = nullptr, featureRender = nullptr, renderLayerOriginal = nullptr,
              vertexEmit = nullptr, armOriginal = nullptr, armAngleOriginal = nullptr;
    jmethodID push = nullptr, pop = nullptr, translate = nullptr, scale = nullptr, rotate = nullptr, quatCtor = nullptr,
              quatRotation = nullptr, rest = nullptr, restX = nullptr, restY = nullptr, restZ = nullptr;
    jmethodID entryPos = nullptr, entryNormal = nullptr, matGet = nullptr, normalGet = nullptr;
    jclass quat = nullptr;
    jfieldID stateWalk = nullptr, stateSpeed = nullptr, stateAge = nullptr, stateBody = nullptr, stateCrouch = nullptr,
             stateRiding = nullptr;
} C;
struct Mount {
    jobject renderer, original, ours;
};
std::vector<Mount> mounts;
struct Texture {
    jobject id = nullptr, solid = nullptr, decal = nullptr, ghost = nullptr;
};
Texture textures[7];
struct Springs {
    int model = -1;
    double time = 0;
    float body = 0, turn = 0;
    std::vector<std::array<float, 6>> spring;
};
std::unordered_map<int, Springs> springs;
bool initialized = false, failed = false, stopping = false;
std::string error = "Preparing native characters";
double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
jclass globalClass(JNIEnv *e, const char *n) {
    auto local = jni::cls(e, n);
    auto result = static_cast<jclass>(e->NewGlobalRef(local));
    e->DeleteLocalRef(local);
    jni::check(e);
    return result;
}
jclass define(JNIEnv *e, jobject loader, const char *n, int resource) {
    auto existing = jvm::findClassAnyLoader(e, n);
    if (e->ExceptionCheck())
        e->ExceptionClear();
    if (existing) {
        auto result = static_cast<jclass>(e->NewGlobalRef(existing));
        e->DeleteLocalRef(existing);
        return result;
    }
    auto bytes = characterResource(resource);
    if (bytes.empty())
        throw std::runtime_error("Missing native character bridge");
    auto c = e->DefineClass(n, loader, reinterpret_cast<const jbyte *>(bytes.data()), static_cast<jsize>(bytes.size()));
    jni::check(e);
    if (!c)
        throw std::runtime_error("Cannot define character bridge");
    auto g = static_cast<jclass>(e->NewGlobalRef(c));
    e->DeleteLocalRef(c);
    return g;
}
bool wears(JNIEnv *e, jobject state) {
    if (!character.enabled || !initialized || failed || stopping || !state || !e->IsInstanceOf(state, C.state))
        return false;
    int id = e->GetIntField(state, C.id);
    auto mc = jni::mc(e), self = jni::player(e, mc);
    if (!self)
        return false;
    auto ec = jni::cls(e, "net/minecraft/class_1297");
    if (id == e->CallIntMethod(self, jni::method(e, ec, "method_5628", "()I")))
        return true;
    if (!character.friends)
        return false;
    auto world = jni::world(e, mc);
    if (!world)
        return false;
    auto other = e->CallObjectMethod(
        world, jni::method(e, jni::cls(e, "net/minecraft/class_638"), "method_8469", "(I)Lnet/minecraft/class_1297;"),
        id);
    jni::check(e);
    return other && social::isFriend(e, other);
}
void rotate(JNIEnv *e, jobject stack, float x, float y, float z) {
    auto q = e->NewObject(C.quat, C.quatCtor);
    e->CallObjectMethod(q, C.quatRotation, z, y, x);
    e->CallVoidMethod(stack, C.rotate, q);
    e->DeleteLocalRef(q);
}
void tr(JNIEnv *e, jobject stack, float x, float y, float z) {
    e->CallVoidMethod(stack, C.translate, x, y, z);
}
float soft(float radians, float limit) {
    if (limit <= 0)
        return radians;
    float max = limit * math::deg;
    return max * std::tanh(radians / max);
}
std::array<float, 3> rotation(std::array<float, 3> v, float x, float y, float z) {
    float c = std::cos(x), s = std::sin(x), a = v[1] * c - v[2] * s, b = v[1] * s + v[2] * c;
    v[1] = a;
    v[2] = b;
    c = std::cos(y);
    s = std::sin(y);
    a = v[0] * c + v[2] * s;
    b = -v[0] * s + v[2] * c;
    v[0] = a;
    v[2] = b;
    c = std::cos(z);
    s = std::sin(z);
    a = v[0] * c - v[1] * s;
    b = v[0] * s + v[1] * c;
    v[0] = a;
    v[1] = b;
    return v;
}
void JNICALL emit(JNIEnv *e, jobject self, jobject pose, jobject out) {
    NativeCallback callback;
    if (e->PushLocalFrame(32) < 0)
        return;
    try {
        auto key = e->GetLongField(self, C.key);
        bool ghost = key >= 10000000;
        key %= 10000000;
        int model = static_cast<int>(key / 100000), detail = static_cast<int>((key / 10000) % 10);
        int partIndex = static_cast<int>((key % 10000) / 3), surfaceIndex = static_cast<int>((key % 10000) % 3);
        const auto &m = characterModel(model, detail);
        if (partIndex < 0 || partIndex >= static_cast<int>(m.parts.size()))
            throw std::runtime_error("Invalid queued character part");
        const auto &p = m.parts[partIndex];
        const auto &s = p.surfaces[surfaceIndex];
        float args[5];
        for (int i = 0; i < 5; ++i)
            args[i] = e->GetFloatField(self, C.args[i]);
        int light = e->GetIntField(self, C.light), overlay = e->GetIntField(self, C.overlay);
        auto matrix = e->CallObjectMethod(pose, C.entryPos), normal = e->CallObjectMethod(pose, C.entryNormal);
        auto a = e->NewFloatArray(16), b = e->NewFloatArray(9);
        e->CallObjectMethod(matrix, C.matGet, a);
        e->CallObjectMethod(normal, C.normalGet, b);
        float mv[16], nm[9];
        e->GetFloatArrayRegion(a, 0, 16, mv);
        e->GetFloatArrayRegion(b, 0, 9, nm);
        jni::check(e);
        std::vector<std::array<float, 6>> transformed(s.pos.size() / 3);
        for (std::size_t i = 0; i < transformed.size(); ++i) {
            std::array<float, 3> v{s.pos[i * 3], s.pos[i * 3 + 1], s.pos[i * 3 + 2]},
                n{s.nrm[i * 3], s.nrm[i * 3 + 1], s.nrm[i * 3 + 2]};
            if (!s.joint.empty()) {
                float k = std::round(s.joint[i] * 16) / 16;
                for (int j = 0; j < 3; ++j)
                    v[j] -= p.joint[j];
                v = rotation(v, args[3] * k, args[4] * k, 0);
                n = rotation(n, args[3] * k, args[4] * k, 0);
                for (int j = 0; j < 3; ++j)
                    v[j] += p.joint[j];
            }
            if (!s.weight.empty()) {
                float k = std::round(s.weight[i] * 16) / 16;
                v = rotation(v, args[0] * k, args[1] * k, args[2] * k);
                n = rotation(n, args[0] * k, args[1] * k, args[2] * k);
            }
            auto &t = transformed[i];
            for (int r = 0; r < 3; ++r) {
                t[r] = mv[12 + r];
                t[r + 3] = 0;
                for (int k = 0; k < 3; ++k) {
                    t[r] += mv[k * 4 + r] * v[k];
                    t[r + 3] += nm[k * 3 + r] * n[k];
                }
            }
            float len = std::sqrt(t[3] * t[3] + t[4] * t[4] + t[5] * t[5]);
            if (len > 1e-6f)
                for (int r = 3; r < 6; ++r)
                    t[r] /= len;
        }
        for (auto i : s.quads) {
            const auto &v = transformed[i];
            e->CallVoidMethod(out, C.vertexEmit, v[0], v[1], v[2],
                              static_cast<jint>(ghost ? (s.color[i] & 0xFFFFFFu) | 0x26000000u : s.color[i]),
                              s.uv[i * 2], s.uv[i * 2 + 1], overlay, (!s.glow.empty() && s.glow[i]) ? 15728880 : light,
                              v[3], v[4], v[5]);
            if (e->ExceptionCheck())
                break;
        }
    } catch (...) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
    }
    e->PopLocalFrame(nullptr);
}
Texture &texture(JNIEnv *e, int model) {
    auto &t = textures[model];
    if (t.id)
        return t;
    auto data = characterResource(2100 + model);
    auto bytes = e->NewByteArray(static_cast<jsize>(data.size()));
    e->SetByteArrayRegion(bytes, 0, static_cast<jsize>(data.size()), reinterpret_cast<const jbyte *>(data.data()));
    auto ic = jni::cls(e, "net/minecraft/class_1011");
    auto image =
        e->CallStaticObjectMethod(ic, jni::smethod(e, ic, "method_49277", "([B)Lnet/minecraft/class_1011;"), bytes);
    jni::check(e);
    auto tc = jni::cls(e, "net/minecraft/class_1043");
    auto label = e->NewObject(C.label, jni::method(e, C.label, "<init>", "()V"));
    auto tex = e->NewObject(
        tc, jni::method(e, tc, "<init>", "(Ljava/util/function/Supplier;Lnet/minecraft/class_1011;)V"), label, image);
    jni::check(e);
    auto idc = jni::cls(e, "net/minecraft/class_2960");
    auto path = std::string("native_character/") + std::to_string(model);
    auto id = e->CallStaticObjectMethod(
        idc, jni::smethod(e, idc, "method_60655", "(Ljava/lang/String;Ljava/lang/String;)Lnet/minecraft/class_2960;"),
        e->NewStringUTF("wrath"), e->NewStringUTF(path.c_str()));
    auto mc = jni::mc(e);
    auto tm = e->CallObjectMethod(
        mc, jni::method(e, jni::cls(e, "net/minecraft/class_310"), "method_1531", "()Lnet/minecraft/class_1060;"));
    e->CallVoidMethod(tm,
                      jni::method(e, jni::cls(e, "net/minecraft/class_1060"), "method_4616",
                                  "(Lnet/minecraft/class_2960;Lnet/minecraft/class_1044;)V"),
                      id, tex);
    jni::check(e);
    auto lc = jni::cls(e, "net/minecraft/class_12249");
    auto layer = [&](const char *n) {
        auto l = e->CallStaticObjectMethod(
            lc, jni::smethod(e, lc, n, "(Lnet/minecraft/class_2960;)Lnet/minecraft/class_1921;"), id);
        jni::check(e);
        return e->NewGlobalRef(l);
    };
    t.solid = layer("method_75982");
    t.decal = layer("method_76000");
    t.ghost = layer("method_75998");
    t.id = e->NewGlobalRef(id);
    return t;
}
struct Pose {
    float pitch[6]{}, yaw[6]{}, roll[6]{};
    jobject bones[6]{};
    float walk = 0, speed = 0, age = 0, body = 0;
    bool crouch = false, riding = false;
};
std::vector<std::array<float, 3>> motions(const CharacterModel &m, int model, int id, const Pose &p) {
    auto &st = springs[id];
    double now = nowSeconds(), dt = st.time ? now - st.time : 0;
    if (st.model != model || st.spring.size() != m.parts.size()) {
        st = {};
        st.model = model;
        st.spring.resize(m.parts.size());
        dt = 0;
        st.body = p.body;
    }
    float turn = std::remainder(p.body - st.body, 360.f);
    st.body = p.body;
    st.time = now;
    st.turn = dt > 0 && dt < .25
                  ? st.turn + (turn / static_cast<float>(dt) - st.turn) * std::min(1.f, static_cast<float>(dt) * 12)
                  : 0;
    std::vector<std::array<float, 3>> result(m.parts.size());
    for (std::size_t i = 0; i < m.parts.size(); ++i) {
        auto &part = m.parts[i];
        if (!part.hasMotion)
            continue;
        const auto &q = part.motion;
        float speed = std::clamp(p.speed, 0.f, 1.f), idle = std::sin(p.age / 20 * q[6] * 2 * math::pi + q[7]),
              walk = std::sin(p.walk * .6662f * q[11] + q[12]) * speed, rate = std::clamp(st.turn / 180, -1.5f, 1.5f);
        std::array<float, 3> target = {(q[0] + q[3] * idle + q[8] * walk + q[13] * speed) * math::deg -
                                           q[14] * p.pitch[part.bone],
                                       (q[1] + q[4] * idle + q[9] * walk + q[16] * rate) * math::deg,
                                       (q[2] + q[5] * idle + q[10] * walk + q[15] * rate) * math::deg};
        if (part.follow >= 0 && part.follow < 6) {
            target[0] += soft(q[17] * (p.pitch[part.follow] - p.pitch[part.bone]), q[18]);
            target[1] += soft(q[17] * (-p.yaw[part.follow] + p.yaw[part.bone]), q[18]);
            target[2] += soft(q[17] * (-p.roll[part.follow] + p.roll[part.bone]), q[18]);
        }
        for (auto &x : target)
            x = std::clamp(x, -q[22] * math::deg, q[22] * math::deg);
        auto &state = st.spring[i];
        if (q[19] <= 0 || dt <= 0 || dt > .25) {
            for (int j = 0; j < 3; ++j) {
                state[j] = target[j];
                state[j + 3] = 0;
            }
        } else {
            int steps = std::max(1, static_cast<int>(std::ceil(dt * 120)));
            float h = static_cast<float>(dt) / steps;
            for (int n = 0; n < steps; ++n)
                for (int j = 0; j < 3; ++j) {
                    state[j + 3] += (q[19] * (target[j] - state[j]) - q[20] * state[j + 3]) * h;
                    state[j] += state[j + 3] * h;
                }
        }
        result[i] = {state[0], state[1], state[2]};
    }
    if (springs.size() > 128)
        for (auto it = springs.begin(); it != springs.end();)
            if (now - it->second.time > 20)
                it = springs.erase(it);
            else
                ++it;
    return result;
}
void chain(JNIEnv *e, jobject stack, const CharacterModel &m, int index, const Pose &p,
           const std::vector<std::array<float, 3>> &angles, bool rotateSelf) {
    const auto &part = m.parts[index];
    if (part.extra) {
        chain(e, stack, m, part.parent, p, angles, true);
        const auto &parent = m.parts[part.parent];
        tr(e, stack, part.pivot[0] - parent.pivot[0], part.pivot[1] - parent.pivot[1], part.pivot[2] - parent.pivot[2]);
        if (rotateSelf && part.hasMotion)
            rotate(e, stack, angles[index][0], angles[index][1], angles[index][2]);
    } else {
        auto b = p.bones[part.bone];
        auto rest = e->CallObjectMethod(b, C.rest);
        float dx = e->GetFloatField(b, C.px) - e->CallFloatMethod(rest, C.restX),
              dy = e->GetFloatField(b, C.py) - e->CallFloatMethod(rest, C.restY),
              dz = e->GetFloatField(b, C.pz) - e->CallFloatMethod(rest, C.restZ);
        tr(e, stack, (m.scale * part.pivot[0] + dx * m.scale) / 16, (24 - m.scale * part.pivot[1] + dy * m.scale) / 16,
           (-m.scale * part.pivot[2] + dz * m.scale) / 16);
        rotate(e, stack, p.pitch[part.bone], p.yaw[part.bone], p.roll[part.bone]);
        e->CallVoidMethod(stack, C.scale, e->GetFloatField(b, C.sx), e->GetFloatField(b, C.sy),
                          e->GetFloatField(b, C.sz));
        float k = m.scale / 16;
        e->CallVoidMethod(stack, C.scale, k, -k, -k);
        e->DeleteLocalRef(rest);
    }
}
bool hiddenLayer(JNIEnv *e, jobject delegate) {
    if (!delegate)
        return false;
    const char *names[] = {"net/minecraft/class_970",  "net/minecraft/class_972", "net/minecraft/class_979",
                           "net/minecraft/class_4507", "net/minecraft/class_976", "net/minecraft/class_983",
                           "net/minecraft/class_978"};
    for (auto name : names) {
        auto c = jni::cls(e, name);
        bool match = e->IsInstanceOf(delegate, c);
        e->DeleteLocalRef(c);
        if (match)
            return true;
    }
    return !character.held && e->IsInstanceOf(delegate, jni::cls(e, "net/minecraft/class_989"));
}
void JNICALL renderFeature(JNIEnv *e, jobject self, jobject stack, jobject queue, jint light, jobject state, jfloat y,
                           jfloat x) {
    NativeCallback callback;
    int matrixDepth = 0;
    if (e->PushLocalFrame(512) < 0)
        return;
    try {
        auto delegate = e->GetObjectField(self, C.delegate);
        bool selected = wears(e, state);
        if (delegate) {
            if (!selected || !hiddenLayer(e, delegate))
                e->CallVoidMethod(delegate, C.featureRender, stack, queue, light, state, y, x);
            e->PopLocalFrame(nullptr);
            return;
        }
        if (!selected) {
            e->PopLocalFrame(nullptr);
            return;
        }
        int mi = character.model, di = character.detail;
        const auto &m = characterModel(mi, di);
        auto &t = texture(e, mi);
        auto owner = e->GetObjectField(self, C.owner), model = e->GetObjectField(owner, C.model);
        Pose p;
        p.walk = e->GetFloatField(state, C.stateWalk);
        p.speed = e->GetFloatField(state, C.stateSpeed);
        p.age = e->GetFloatField(state, C.stateAge);
        p.body = e->GetFloatField(state, C.stateBody);
        p.crouch = e->GetBooleanField(state, C.stateCrouch);
        p.riding = e->GetBooleanField(state, C.stateRiding);
        for (int i = 0; i < 6; ++i) {
            p.bones[i] = e->GetObjectField(model, C.bone[i]);
            p.pitch[i] = e->GetFloatField(p.bones[i], C.pitch);
            p.yaw[i] = e->GetFloatField(p.bones[i], C.yaw);
            p.roll[i] = e->GetFloatField(p.bones[i], C.roll);
        }
        for (auto &part : m.parts)
            if (!part.extra && part.bone >= 2) {
                float phase = p.walk * .6662f + ((part.bone == 2 || part.bone == 5) ? math::pi : 0),
                      walk = std::cos(phase) * p.speed * (part.bone >= 4 ? 1.4f : 1);
                p.pitch[part.bone] += soft(walk * part.swing, part.limit) - walk;
                if (p.crouch && part.bone >= 4 && !p.riding)
                    p.pitch[part.bone] -= .5f;
            }
        auto angles = motions(m, mi * 3 + di, e->GetIntField(state, C.id), p);
        auto submit = jni::method(
            e, jni::cls(e, "net/minecraft/class_11785"), "method_73483",
            "(Lnet/minecraft/class_4587;Lnet/minecraft/class_1921;Lnet/minecraft/class_11659$class_11660;)V");
        int overlay = e->CallStaticIntMethod(
            C.renderer, jni::smethod(e, C.renderer, "method_23622", "(Lnet/minecraft/class_10042;F)I"), state, 0.f);
        jni::check(e);
        bool ghost = e->GetBooleanField(state, jni::field(e, C.state, "field_53333", "Z"));
        if (ghost && e->GetBooleanField(state, jni::field(e, C.state, "field_53461", "Z"))) {
            e->PopLocalFrame(nullptr);
            return;
        }
        e->CallVoidMethod(stack, C.push);
        ++matrixDepth;
        auto root = e->CallObjectMethod(model, jni::method(e, jni::cls(e, "net/minecraft/class_583"), "method_63512",
                                                           "()Lnet/minecraft/class_630;"));
        e->CallVoidMethod(root, jni::method(e, C.part, "method_22703", "(Lnet/minecraft/class_4587;)V"), stack);
        if (p.riding)
            tr(e, stack, 0, -(12 - m.scale * m.hip) / 16, 0);
        for (std::size_t i = 0; i < m.parts.size(); ++i) {
            if (e->PushLocalFrame(64) < 0)
                break;
            auto &part = m.parts[i];
            bool bends = part.hasMotion && part.motion[21] > 0;
            e->CallVoidMethod(stack, C.push);
        ++matrixDepth;
            chain(e, stack, m, static_cast<int>(i), p, angles, !bends);
            float bent = 0, turn = 0;
            if (part.neck) {
                float max = part.lookLimit * math::deg;
                bent = std::clamp(part.lookPitch * p.pitch[0], -max, max);
                turn = std::clamp(-part.lookYaw * p.yaw[0], -max, max);
            } else if (part.bone >= 4) {
                float phase = p.walk * .6662f + (part.bone == 5 ? math::pi : 0);
                bent = p.riding
                           ? .3f
                           : std::min(1.7f, std::min(1.f, p.speed) * (.1f + .85f * std::max(0.f, std::sin(phase))) +
                                                (p.crouch ? .62f : 0));
            } else if (part.bone >= 2)
                bent = -std::min(1.6f, .16f + .3f * std::min(1.f, p.speed) + (p.crouch ? .2f : 0));
            for (int s = 0; s < 3; ++s)
                if (!part.surfaces[s].pos.empty() && (!ghost || s == 0)) {
                    jlong key = (ghost ? 10000000 : 0) + mi * 100000 + di * 10000 + static_cast<int>(i) * 3 + s;
                    auto draw = e->NewObject(C.draw, C.drawCtor, key, light, overlay, bends ? angles[i][0] : 0,
                                             bends ? angles[i][1] : 0, bends ? angles[i][2] : 0, bent, turn);
                    e->CallVoidMethod(queue, submit, stack, ghost ? t.ghost : (s == 2 ? t.decal : t.solid), draw);
                }
            e->CallVoidMethod(stack, C.pop);
        --matrixDepth;
            e->PopLocalFrame(nullptr);
            jni::check(e);
        }
        e->CallVoidMethod(stack, C.pop);
        --matrixDepth;
        jni::check(e);
    } catch (const std::exception &ex) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
        while (matrixDepth > 0) { e->CallVoidMethod(stack, C.pop); --matrixDepth; }
        failed = true;
        error = std::string("Character render failed: ") + ex.what();
        static bool logged = false;
        if (!logged) {
            logged = true;
            VLOG_WARN("character", "Render failed: {}", ex.what());
        }
    }
    e->PopLocalFrame(nullptr);
}
void chainHand(JNIEnv *e, jobject stack, const CharacterModel &m, int index, jobject arm) {
    const auto &p = m.parts[index];
    if (p.extra) {
        chainHand(e, stack, m, p.parent, arm);
        const auto &parent = m.parts[p.parent];
        tr(e, stack, p.pivot[0] - parent.pivot[0], p.pivot[1] - parent.pivot[1], p.pivot[2] - parent.pivot[2]);
    } else {
        tr(e, stack, e->GetFloatField(arm, C.px) / 16, e->GetFloatField(arm, C.py) / 16,
           e->GetFloatField(arm, C.pz) / 16);
        rotate(e, stack, e->GetFloatField(arm, C.pitch), e->GetFloatField(arm, C.yaw), e->GetFloatField(arm, C.roll));
        float side = p.bone == 2 ? -1.f : 1.f;
        tr(e, stack, (side - (p.grip[0] - p.pivot[0]) * m.scale) / 16, (10 + (p.grip[1] - p.pivot[1]) * m.scale) / 16,
           (-2 + (p.grip[2] - p.pivot[2]) * m.scale) / 16);
        float k = m.scale / 16;
        e->CallVoidMethod(stack, C.scale, k, -k, -k);
    }
}
void JNICALL firstPersonArm(JNIEnv *e, jobject self, jobject stack, jobject queue, jint light, jobject skin,
                            jobject arm, jboolean sleeve) {
    NativeCallback callback;
    bool handled = false;
    int depth = 0;
    if (character.enabled && !failed && !stopping && initialized && e->PushLocalFrame(128) >= 0) {
        try {
            int mi = character.model, di = character.detail;
            const auto &m = characterModel(mi, di);
            if (m.arms) {
                auto &t = texture(e, mi);
                auto model = e->GetObjectField(self, C.model);
                auto right = e->GetObjectField(model, C.bone[2]);
                int bone = e->IsSameObject(right, arm) ? 2 : 3;
                auto reset = jni::method(e, C.part, "method_41923", "()V");
                e->CallVoidMethod(arm, reset);
                e->SetFloatField(arm, C.roll, bone == 2 ? .1f : -.1f);
                auto submit = jni::method(
                    e, jni::cls(e, "net/minecraft/class_11785"), "method_73483",
                    "(Lnet/minecraft/class_4587;Lnet/minecraft/class_1921;Lnet/minecraft/class_11659$class_11660;)V");
                for (std::size_t i = 0; i < m.parts.size(); ++i)
                    if (m.parts[i].bone == bone) {
                        e->CallVoidMethod(stack, C.push);
                        ++depth;
                        chainHand(e, stack, m, static_cast<int>(i), arm);
                        for (int surf = 0; surf < 3; ++surf)
                            if (!m.parts[i].surfaces[surf].pos.empty()) {
                                jlong key = mi * 100000 + di * 10000 + static_cast<int>(i) * 3 + surf;
                                auto draw =
                                    e->NewObject(C.draw, C.drawCtor, key, light, 0xA0000, 0.f, 0.f, 0.f, 0.f, 0.f);
                                e->CallVoidMethod(queue, submit, stack, surf == 2 ? t.decal : t.solid, draw);
                                e->DeleteLocalRef(draw);
                            }
                        e->CallVoidMethod(stack, C.pop);
                        --depth;
                        jni::check(e);
                    }
                handled = true;
            }
        } catch (...) {
            if (e->ExceptionCheck())
                e->ExceptionClear();
            while (depth > 0) {
                e->CallVoidMethod(stack, C.pop);
                --depth;
            }
        }
        e->PopLocalFrame(nullptr);
    }
    if (!handled)
        e->CallNonvirtualVoidMethod(self, C.avatar, C.armOriginal, stack, queue, light, skin, arm, sleeve);
}
void JNICALL armAngle(JNIEnv *e, jobject self, jobject state, jobject arm, jobject stack) {
    NativeCallback callback;
    bool handled = false;
    if (e->PushLocalFrame(96) >= 0) {
        try {
            if (wears(e, state)) {
                const auto &m = characterModel(character.model, character.detail);
                auto ac = jni::cls(e, "net/minecraft/class_1306");
                auto right =
                    e->GetStaticObjectField(ac, e->GetStaticFieldID(ac, "field_6183", "Lnet/minecraft/class_1306;"));
                int bone = e->IsSameObject(arm, right) ? 2 : 3;
                const CharacterPart *part = nullptr;
                for (auto &p : m.parts)
                    if (!p.extra && p.bone == bone) {
                        part = &p;
                        break;
                    }
                if (part) {
                    auto vp = e->GetObjectField(self, C.bone[bone]);
                    auto root = e->CallObjectMethod(self, jni::method(e, jni::cls(e, "net/minecraft/class_583"),
                                                                      "method_63512", "()Lnet/minecraft/class_630;"));
                    e->CallVoidMethod(root, jni::method(e, C.part, "method_22703", "(Lnet/minecraft/class_4587;)V"),
                                      stack);
                    if (e->GetBooleanField(state, C.stateRiding))
                        tr(e, stack, 0, -(12 - m.scale * m.hip) / 16, 0);
                    auto rest = e->CallObjectMethod(vp, C.rest);
                    float dx = e->GetFloatField(vp, C.px) - e->CallFloatMethod(rest, C.restX),
                          dy = e->GetFloatField(vp, C.py) - e->CallFloatMethod(rest, C.restY),
                          dz = e->GetFloatField(vp, C.pz) - e->CallFloatMethod(rest, C.restZ);
                    tr(e, stack, (part->pivot[0] + dx) * m.scale / 16,
                       (24 - part->pivot[1] * m.scale + dy * m.scale) / 16, (-part->pivot[2] + dz) * m.scale / 16);
                    rotate(e, stack, e->GetFloatField(vp, C.pitch), e->GetFloatField(vp, C.yaw),
                           e->GetFloatField(vp, C.roll));
                    tr(e, stack, (part->grip[0] - part->pivot[0]) * m.scale / 16,
                       -(part->grip[1] - part->pivot[1]) * m.scale / 16,
                       -(part->grip[2] - part->pivot[2]) * m.scale / 16);
                    e->CallVoidMethod(stack, C.scale, m.scale, m.scale, m.scale);
                    tr(e, stack, (bone == 2 ? 1.f : -1.f) / 16, -.625f, .125f);
                    jni::check(e);
                    handled = true;
                }
            }
        } catch (...) {
            if (e->ExceptionCheck())
                e->ExceptionClear();
        }
        e->PopLocalFrame(nullptr);
    }
    if (!handled)
        e->CallNonvirtualVoidMethod(self, C.playerModel, C.armAngleOriginal, state, arm, stack);
}
jobject JNICALL getLayer(JNIEnv *e, jobject self, jobject state, jboolean body, jboolean translucent, jboolean glow) {
    NativeCallback callback;
    bool selected = false;
    if (e->PushLocalFrame(32) >= 0) {
        try {
            selected = wears(e, state);
        } catch (...) {
            if (e->ExceptionCheck())
                e->ExceptionClear();
        }
        e->PopLocalFrame(nullptr);
    }
    if (selected)
        return nullptr;
    return e->CallNonvirtualObjectMethod(self, C.renderer, C.renderLayerOriginal, state, body, translucent, glow);
}
void init(JNIEnv *e) {
    auto mcClass = jni::cls(e, "net/minecraft/class_310"), cc = jni::cls(e, "java/lang/Class");
    auto loader = e->CallObjectMethod(mcClass, jni::method(e, cc, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    C.layer = define(e, loader, "dev/wrath/NativeCharacterLayer", 2200);
    C.draw = define(e, loader, "dev/wrath/NativeCharacterDraw", 2201);
    C.label = define(e, loader, "dev/wrath/NativeTextureName", 2202);
    JNINativeMethod feature{
        const_cast<char *>("method_4199"),
        const_cast<char *>("(Lnet/minecraft/class_4587;Lnet/minecraft/class_11659;ILnet/minecraft/class_10017;FF)V"),
        reinterpret_cast<void *>(&renderFeature)},
        draw{const_cast<char *>("render"),
             const_cast<char *>("(Lnet/minecraft/class_4587$class_4665;Lnet/minecraft/class_4588;)V"),
             reinterpret_cast<void *>(&emit)};
    if (e->RegisterNatives(C.layer, &feature, 1) != JNI_OK || e->RegisterNatives(C.draw, &draw, 1) != JNI_OK)
        throw std::runtime_error("Cannot register native character renderer");
    C.renderer = globalClass(e, "net/minecraft/class_922");
    C.stack = globalClass(e, "net/minecraft/class_4587");
    C.part = globalClass(e, "net/minecraft/class_630");
    C.state = globalClass(e, "net/minecraft/class_10055");
    C.vertex = globalClass(e, "net/minecraft/class_4588");
    C.quat = globalClass(e, "org/joml/Quaternionf");
    C.features = jni::field(e, C.renderer, "field_4738", "Ljava/util/List;");
    C.model = jni::field(e, C.renderer, "field_4737", "Lnet/minecraft/class_583;");
    C.delegate = jni::field(e, C.layer, "delegate", "Lnet/minecraft/class_3887;");
    C.owner = jni::field(e, C.layer, "owner", "Lnet/minecraft/class_3883;");
    C.id = jni::field(e, C.state, "field_53528", "I");
    C.key = jni::field(e, C.draw, "key", "J");
    C.light = jni::field(e, C.draw, "light", "I");
    C.overlay = jni::field(e, C.draw, "overlay", "I");
    const char *an[] = {"pitch", "yaw", "roll", "bent", "turn"};
    for (int i = 0; i < 5; ++i)
        C.args[i] = jni::field(e, C.draw, an[i], "F");
    auto pm = jni::cls(e, "net/minecraft/class_591");
    const char *bn[] = {"field_3398", "field_3391", "field_3401", "field_27433", "field_3392", "field_3397"};
    for (int i = 0; i < 6; ++i)
        C.bone[i] = jni::field(e, pm, bn[i], "Lnet/minecraft/class_630;");
    C.px = jni::field(e, C.part, "field_3657", "F");
    C.py = jni::field(e, C.part, "field_3656", "F");
    C.pz = jni::field(e, C.part, "field_3655", "F");
    C.pitch = jni::field(e, C.part, "field_3654", "F");
    C.yaw = jni::field(e, C.part, "field_3675", "F");
    C.roll = jni::field(e, C.part, "field_3674", "F");
    C.sx = jni::field(e, C.part, "field_37938", "F");
    C.sy = jni::field(e, C.part, "field_37939", "F");
    C.sz = jni::field(e, C.part, "field_37940", "F");
    C.stateWalk = jni::field(e, C.state, "field_53450", "F");
    C.stateSpeed = jni::field(e, C.state, "field_53451", "F");
    C.stateAge = jni::field(e, C.state, "field_53328", "F");
    C.stateBody = jni::field(e, C.state, "field_53446", "F");
    C.stateCrouch = jni::field(e, C.state, "field_53410", "Z");
    C.stateRiding = jni::field(e, C.state, "field_53413", "Z");
    C.layerCtor = jni::method(e, C.layer, "<init>", "(Lnet/minecraft/class_3883;Lnet/minecraft/class_3887;)V");
    C.drawCtor = jni::method(e, C.draw, "<init>", "(JIIFFFFF)V");
    C.featureRender =
        jni::method(e, jni::cls(e, "net/minecraft/class_3887"), "method_4199",
                    "(Lnet/minecraft/class_4587;Lnet/minecraft/class_11659;ILnet/minecraft/class_10017;FF)V");
    C.vertexEmit = jni::method(e, C.vertex, "method_23919", "(FFFIFFIIFFF)V");
    C.push = jni::method(e, C.stack, "method_22903", "()V");
    C.pop = jni::method(e, C.stack, "method_22909", "()V");
    C.translate = jni::method(e, C.stack, "method_46416", "(FFF)V");
    C.scale = jni::method(e, C.stack, "method_22905", "(FFF)V");
    C.rotate = jni::method(e, C.stack, "method_22907", "(Lorg/joml/Quaternionf;)V");
    C.quatCtor = jni::method(e, C.quat, "<init>", "()V");
    C.quatRotation = jni::method(e, C.quat, "rotationZYX", "(FFF)Lorg/joml/Quaternionf;");
    C.rest = jni::method(e, C.part, "method_41921", "()Lnet/minecraft/class_5603;");
    auto rest = jni::cls(e, "net/minecraft/class_5603");
    C.restX = jni::method(e, rest, "comp_2997", "()F");
    C.restY = jni::method(e, rest, "comp_2998", "()F");
    C.restZ = jni::method(e, rest, "comp_2999", "()F");
    auto ep = jni::cls(e, "net/minecraft/class_4587$class_4665");
    C.entryPos = jni::method(e, ep, "method_23761", "()Lorg/joml/Matrix4f;");
    C.entryNormal = jni::method(e, ep, "method_23762", "()Lorg/joml/Matrix3f;");
    C.matGet = jni::method(e, jni::cls(e, "org/joml/Matrix4f"), "get", "([F)[F");
    C.normalGet = jni::method(e, jni::cls(e, "org/joml/Matrix3f"), "get", "([F)[F");
    auto method =
        jni::method(e, C.renderer, "method_24302", "(Lnet/minecraft/class_10042;ZZZ)Lnet/minecraft/class_1921;");
    if (JNIHook_Attach(method, reinterpret_cast<void *>(&getLayer), &C.renderLayerOriginal) != JNIHOOK_OK)
        throw std::runtime_error("Character model hook unavailable");
    C.avatar = globalClass(e, "net/minecraft/class_1007");
    C.playerModel = globalClass(e, "net/minecraft/class_591");
    auto hand = jni::method(e, C.avatar, "method_23205",
                            "(Lnet/minecraft/class_4587;Lnet/minecraft/class_11659;ILnet/minecraft/class_2960;Lnet/"
                            "minecraft/class_630;Z)V");
    if (JNIHook_Attach(hand, reinterpret_cast<void *>(&firstPersonArm), &C.armOriginal) != JNIHOOK_OK)
        throw std::runtime_error("Character hand hook unavailable");
    auto angle = jni::method(e, C.playerModel, "method_72859",
                             "(Lnet/minecraft/class_10055;Lnet/minecraft/class_1306;Lnet/minecraft/class_4587;)V");
    if (JNIHook_Attach(angle, reinterpret_cast<void *>(&armAngle), &C.armAngleOriginal) != JNIHOOK_OK)
        throw std::runtime_error("Character held-item hook unavailable");
    initialized = true;
    error = "Native 3D characters";
}
} // namespace
void installCharacter(JNIEnv *e) {
    if (failed || stopping)
        return;
    try {
        if (!initialized)
            init(e);
        auto mc = jni::mc(e);
        auto dispatcher = e->CallObjectMethod(
            mc, jni::method(e, jni::cls(e, "net/minecraft/class_310"), "method_1561", "()Lnet/minecraft/class_898;"));
        auto map = e->GetObjectField(
            dispatcher, jni::field(e, jni::cls(e, "net/minecraft/class_898"), "field_4687", "Ljava/util/Map;"));
        auto values = e->CallObjectMethod(
            map, jni::method(e, jni::cls(e, "java/util/Map"), "values", "()Ljava/util/Collection;"));
        auto iterator = e->CallObjectMethod(
            values, jni::method(e, jni::cls(e, "java/util/Collection"), "iterator", "()Ljava/util/Iterator;"));
        auto ic = jni::cls(e, "java/util/Iterator");
        auto has = jni::method(e, ic, "hasNext", "()Z"), next = jni::method(e, ic, "next", "()Ljava/lang/Object;");
        auto listClass = jni::cls(e, "java/util/ArrayList");
        auto add = jni::method(e, listClass, "add", "(Ljava/lang/Object;)Z");
        auto lc = jni::cls(e, "java/util/List");
        auto size = jni::method(e, lc, "size", "()I"), get = jni::method(e, lc, "get", "(I)Ljava/lang/Object;");
        while (e->CallBooleanMethod(iterator, has)) {
            auto renderer = e->CallObjectMethod(iterator, next);
            bool mounted = false;
            for (auto &m : mounts)
                if (e->IsSameObject(renderer, m.renderer))
                    mounted = true;
            if (mounted) {
                e->DeleteLocalRef(renderer);
                continue;
            }
            auto original = e->GetObjectField(renderer, C.features);
            auto ours = e->NewObject(listClass, jni::method(e, listClass, "<init>", "()V"));
            int n = e->CallIntMethod(original, size);
            for (int i = 0; i < n; ++i) {
                auto f = e->CallObjectMethod(original, get, i);
                auto wrapper = e->NewObject(C.layer, C.layerCtor, renderer, f);
                e->CallBooleanMethod(ours, add, wrapper);
                e->DeleteLocalRef(f);
                e->DeleteLocalRef(wrapper);
            }
            auto custom = e->NewObject(C.layer, C.layerCtor, renderer, nullptr);
            e->CallBooleanMethod(ours, add, custom);
            e->SetObjectField(renderer, C.features, ours);
            jni::check(e);
            mounts.push_back({e->NewGlobalRef(renderer), e->NewGlobalRef(original), e->NewGlobalRef(ours)});
            e->DeleteLocalRef(renderer);
            e->DeleteLocalRef(original);
            e->DeleteLocalRef(ours);
            e->DeleteLocalRef(custom);
        }
        texture(e, character.model);
        characterModel(character.model, character.detail);
        jni::check(e);
    } catch (const std::exception &ex) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
        failed = true;
        error = ex.what();
        VLOG_WARN("character", "Native character setup: {}", error);
    }
}
const char *characterStatus() {
    return error.c_str();
}
bool prepareCharacterShutdown(JNIEnv *e) {
    character.enabled = false;
    stopping = true;
    if (e->PushLocalFrame(64) < 0) {
        blockUnload();
        return false;
    }
    for (auto &m : mounts) {
        auto current = e->GetObjectField(m.renderer, C.features);
        if (!e->IsSameObject(current, m.ours)) {
            blockUnload();
            e->PopLocalFrame(nullptr);
            return false;
        }
        e->DeleteLocalRef(current);
    }
    for (auto &m : mounts) {
        e->SetObjectField(m.renderer, C.features, m.original);
        e->DeleteGlobalRef(m.renderer);
        e->DeleteGlobalRef(m.original);
        e->DeleteGlobalRef(m.ours);
    }
    mounts.clear();
    if (C.layer)
        e->UnregisterNatives(C.layer);
    if (C.draw)
        e->UnregisterNatives(C.draw);
    for (auto &t : textures) {
        if (t.id) {
            try {
                auto mc = jni::mc(e);
                auto tm = e->CallObjectMethod(mc, jni::method(e, jni::cls(e, "net/minecraft/class_310"), "method_1531",
                                                              "()Lnet/minecraft/class_1060;"));
                e->CallVoidMethod(tm,
                                  jni::method(e, jni::cls(e, "net/minecraft/class_1060"), "method_4615",
                                              "(Lnet/minecraft/class_2960;)V"),
                                  t.id);
                jni::check(e);
            } catch (...) {
                if (e->ExceptionCheck())
                    e->ExceptionClear();
            }
        }
        for (auto o : {t.id, t.solid, t.decal, t.ghost})
            if (o)
                e->DeleteGlobalRef(o);
        t = {};
    }
    springs.clear();
    e->PopLocalFrame(nullptr);
    return true;
}
void shutdownCharacter(JNIEnv *e) {
    if (!stopping && !prepareCharacterShutdown(e))
        return;
    for (auto *c : {&C.layer, &C.draw, &C.label, &C.renderer, &C.stack, &C.part, &C.state, &C.vertex, &C.quat,
                    &C.avatar, &C.playerModel})
        if (*c) {
            e->DeleteGlobalRef(*c);
            *c = nullptr;
        }
}
} // namespace vanta::modules::visualports
