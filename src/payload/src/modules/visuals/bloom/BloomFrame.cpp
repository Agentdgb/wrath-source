#include "Bloom.h"
#include "BloomCompat.h"
#include "modules/render/targetesp/target_attack.h"
#include "modules/visuals/ports/PortJni.h"
#include "modules/visuals/ports/VisualPorts.h"
#include "util/Log.h"
#include "vgui/bridge.h"
#include <unordered_map>
namespace dt::modules::bloom {
MinecraftClient currentClient;
WorldSettings worldSettings;
TargetSettings targetSettings;
namespace {
using namespace visualports;
NativeWorld sampledWorld;
EntityView sampledSelf;
static JNIEnv *activeEnv = nullptr;
static jobject activeWorld = nullptr, heightType = nullptr;
static jmethodID topMethod = nullptr;
std::string lastError;
double lastFrame = 0;
bool wantsParticles() {
    return worldSettings.enabled && worldSettings.density > 0 &&
           (worldSettings.stars || worldSettings.spark || worldSettings.flame || worldSettings.runes || worldSettings.snow);
}
int targetId = -1;
double targetHit = 0;
std::unordered_map<int, std::unique_ptr<EntityView>> entities;
EntityView *sampleEntity(JNIEnv *e, jobject object, EntityView *selfView = nullptr) {
    if (!object)
        return nullptr;
    auto ec = jni::cls(e, "net/minecraft/class_1297"), lc = jni::cls(e, "net/minecraft/class_1309");
    if (!e->IsInstanceOf(object, lc))
        return nullptr;
    int id = e->CallIntMethod(object, jni::method(e, ec, "method_5628", "()I"));
    EntityView *view = selfView;
    if (!view) {
        auto &entry = entities[id];
        if (!entry) {
            if (entities.size() > 4096)
                throw std::runtime_error("Visual snapshot limit reached");
            entry = std::make_unique<EntityView>();
        }
        view = entry.get();
    }
    view->id = id;
    auto pos = e->CallObjectMethod(object, jni::method(e, ec, "method_30950", "(F)Lnet/minecraft/class_243;"),
                                   RegionPassCache::td);
    auto vc = jni::cls(e, "net/minecraft/class_243");
    view->lastRenderX = view->x = e->GetDoubleField(pos, jni::field(e, vc, "field_1352", "D"));
    view->lastRenderY = view->y = e->GetDoubleField(pos, jni::field(e, vc, "field_1351", "D"));
    view->lastRenderZ = view->z = e->GetDoubleField(pos, jni::field(e, vc, "field_1350", "D"));
    view->height = e->CallFloatMethod(object, jni::method(e, ec, "method_17682", "()F"));
    view->width = e->CallFloatMethod(object, jni::method(e, ec, "method_17681", "()F"));
    view->lastBodyYaw = view->bodyYaw = MathHelper::lerpAngleDegrees(
        RegionPassCache::td, e->GetFloatField(object, jni::field(e, lc, "field_6220", "F")),
        e->GetFloatField(object, jni::field(e, lc, "field_6283", "F")));
    view->lastHeadYaw = view->headYaw = MathHelper::lerpAngleDegrees(
        RegionPassCache::td, e->GetFloatField(object, jni::field(e, lc, "field_6259", "F")),
        e->GetFloatField(object, jni::field(e, lc, "field_6241", "F")));
    view->pitch = e->CallFloatMethod(object, jni::method(e, ec, "method_36455", "()F"));
    auto scale = jni::optional(e, lc, "method_55693", "()F");
    if (scale)
        view->scale = e->CallFloatMethod(object, scale);
    view->dead = !e->CallBooleanMethod(object, jni::method(e, ec, "method_5805", "()Z"));
    view->removed = e->CallBooleanMethod(object, jni::method(e, ec, "method_31481", "()Z"));
    view->sneaking = e->CallBooleanMethod(object, jni::method(e, ec, "method_18276", "()Z"));
    view->spectator = e->CallBooleanMethod(object, jni::method(e, ec, "method_7325", "()Z"));
    view->player = e->IsInstanceOf(object, jni::cls(e, "net/minecraft/class_1657"));
    view->deathTime = e->GetIntField(object, jni::field(e, lc, "field_6213", "I"));
    view->hurtTime = e->GetIntField(object, jni::field(e, lc, "field_6235", "I"));
    jni::check(e);
    return view;
}
RGBA unpack(float f) {
    auto c = Float::floatToRawIntBits(f);
    return {float((c >> 16) & 255) / 255, float((c >> 8) & 255) / 255, float(c & 255) / 255, float(c >> 24) / 255};
}
Point point(float x, float y, float z) {
    return {RegionPassCache::z + x, RegionPassCache::aa + y, RegionPassCache::ab + z};
}
int ground(int x, int z) {
    if (!activeEnv)
        return sampledWorld.bottom;
    int value = activeEnv->CallIntMethod(activeWorld, topMethod, heightType, x, z);
    jni::check(activeEnv);
    return value;
}
} // namespace
MinecraftClient *MinecraftClient::getInstance() { return &currentClient; }
int RegionPassCache::col(int value) {
    if (value >= 0)
        return value;
    float r, g, b, a;
    vgui::bridge::clientAccent(r, g, b, a);
    return static_cast<int>(r * 255) << 16 | static_cast<int>(g * 255) << 8 | static_cast<int>(b * 255);
}
const char *runtimeStatus() { return lastError.empty() ? visualports::worldStatus() : lastError.c_str(); }
bool needsWorld() {
    return wantsParticles() || world::visible() || targetSettings.enabled || halo::visible();
}
void resetWorld() {
    world::d();
    halo::p();
    RegionPassCache::bh = RegionPassCache::ah = false;
    RegionPassCache::aq = nullptr;
    RegionPassCache::clear();
    lastFrame = 0;
    targetId = -1;
    targetHit = 0;
    entities.clear();
    currentClient.world = nullptr;
    currentClient.player = nullptr;
}
void shutdown() {
    worldSettings.enabled = false;
    targetSettings.enabled = false;
    resetWorld();
}
void worldFrame(JNIEnv *e, jobject level, jobject self, const hitfx::render_frame &frame, const Canvas &canvas) {
    if (!needsWorld() || !level || !self)
        return;
    if (e->PushLocalFrame(128) < 0)
        return;
    try {
        auto client = jni::mc(e);
        auto counter = e->CallObjectMethod(client, jni::method(e, jni::cls(e, "net/minecraft/class_310"),
                                                               "method_61966", "()Lnet/minecraft/class_9779;"));
        RegionPassCache::td = std::clamp(
            e->CallFloatMethod(counter, jni::method(e, jni::cls(e, "net/minecraft/class_9779"), "method_60637", "(Z)F"),
                               JNI_FALSE),
            0.f, 1.f);
        jni::check(e);
        auto wc = jni::cls(e, "net/minecraft/class_1937");
        auto ht = jni::cls(e, "net/minecraft/class_2902$class_2903");
        heightType = e->GetStaticObjectField(
            ht, e->GetStaticFieldID(ht, "field_13203", "Lnet/minecraft/class_2902$class_2903;"));
        jni::check(e);
        topMethod = jni::method(e, wc, "method_8624", "(Lnet/minecraft/class_2902$class_2903;II)I");
        sampledWorld.bottom = e->CallIntMethod(level, jni::method(e, wc, "method_31607", "()I"));
        sampledWorld.height = &ground;
        activeEnv = e;
        activeWorld = level;
        currentClient.world = &sampledWorld;
        currentClient.player = &sampledSelf;
        currentClient.gameRenderer.camera.focused = &sampledSelf;
        auto ec = jni::cls(e, "net/minecraft/class_1297"), lc = jni::cls(e, "net/minecraft/class_1309");
        sampledSelf.x = e->CallDoubleMethod(self, jni::method(e, ec, "method_23317", "()D"));
        sampledSelf.y = e->CallDoubleMethod(self, jni::method(e, ec, "method_23318", "()D"));
        sampledSelf.z = e->CallDoubleMethod(self, jni::method(e, ec, "method_23321", "()D"));
        auto sc = jni::cls(e, "net/minecraft/class_1294");
        const char *effects[] = {"field_5919", "field_38092", "field_5916"};
        auto has = jni::method(e, lc, "method_6059", "(Lnet/minecraft/class_6880;)Z");
        for (int i = 0; i < 3; ++i) {
            auto effect =
                e->GetStaticObjectField(sc, e->GetStaticFieldID(sc, effects[i], "Lnet/minecraft/class_6880;"));
            jni::check(e);
            sampledSelf.effects[i] = e->CallBooleanMethod(self, has, effect);
            e->DeleteLocalRef(effect);
        }
        sampledSelf.hurtTime = e->GetIntField(self, jni::field(e, lc, "field_6235", "I"));
        auto renderer = e->GetObjectField(
            client, jni::field(e, jni::cls(e, "net/minecraft/class_310"), "field_1773", "Lnet/minecraft/class_757;"));
        auto cam = e->CallObjectMethod(renderer, jni::method(e, jni::cls(e, "net/minecraft/class_757"), "method_19418",
                                                             "()Lnet/minecraft/class_4184;"));
        auto sub = e->CallObjectMethod(cam, jni::method(e, jni::cls(e, "net/minecraft/class_4184"), "method_19334",
                                                        "()Lnet/minecraft/class_5636;"));
        int ordinal = e->CallIntMethod(sub, jni::method(e, jni::cls(e, "java/lang/Enum"), "ordinal", "()I"));
        currentClient.gameRenderer.camera.submersion = ordinal == 0   ? CameraSubmersionType::LAVA
                                                       : ordinal == 1 ? CameraSubmersionType::WATER
                                                       : ordinal == 2 ? CameraSubmersionType::POWDER_SNOW
                                                                      : CameraSubmersionType::NONE;
        jni::check(e);
        currentClient.window = {frame.viewport_w, frame.viewport_h};
        currentClient.options.fov.value =
            static_cast<int>(2 * std::atan(1 / std::max(.01f, frame.projection[5])) * 180 / Math::PI);
        auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        RegionPassCache::dt = lastFrame ? static_cast<float>(std::clamp(now - lastFrame, 0., .1)) : 0;
        lastFrame = now;
        RegionPassCache::now = now;
        RegionPassCache::ac = std::max(1.f, frame.projection[5] * frame.viewport_h * .5f);
        RegionPassCache::af = RegionPassCache::col(-3);
        RegionPassCache::ag = 0xFFFFFF;
        RegionPassCache::z = frame.cam_x;
        RegionPassCache::aa = frame.cam_y;
        RegionPassCache::ab = frame.cam_z;
        RegionPassCache::fX = -frame.modelview[2];
        RegionPassCache::fY = -frame.modelview[6];
        RegionPassCache::fZ = -frame.modelview[10];
        RegionPassCache::rX = frame.modelview[0];
        RegionPassCache::rZ = frame.modelview[8];
        RegionPassCache::uX = frame.modelview[1];
        RegionPassCache::uZ = frame.modelview[9];
        RegionPassCache::bh = wantsParticles();
        RegionPassCache::bi = worldSettings.stars;
        RegionPassCache::bj = worldSettings.spark;
        RegionPassCache::bk = worldSettings.flame;
        RegionPassCache::bl = worldSettings.runes;
        RegionPassCache::bm = worldSettings.snow;
        RegionPassCache::bn = worldSettings.density;
        auto c = worldSettings.color;
        RegionPassCache::bo = worldSettings.theme ? -3 : (int(c.r * 255) << 16 | int(c.g * 255) << 8 | int(c.b * 255));
        RegionPassCache::clear();
        world::a();
        RegionPassCache::rY = frame.modelview[4];
        RegionPassCache::uY = frame.modelview[5];
        RegionPassCache::am = targetSettings.scale;
        RegionPassCache::an = targetSettings.speed;
        RegionPassCache::ao = targetSettings.opacity;
        RegionPassCache::ap = targetSettings.opacity;
        RegionPassCache::ah = targetSettings.enabled;
        RegionPassCache::ai = targetSettings.glow;
        RegionPassCache::aj = targetSettings.mode;
        RegionPassCache::ak = targetSettings.on;
        auto tc = targetSettings.color;
        RegionPassCache::al = int(tc.r * 255) << 16 | int(tc.g * 255) << 8 | int(tc.b * 255);
        if (targetSettings.enabled) {
            targetesp::attack::install(e);
            double age = 0;
            auto attacked = targetesp::attack::consume(e, &age);
            if (attacked) {
                auto view = sampleEntity(e, attacked);
                if (view) {
                    targetId = view->id;
                    targetHit = now - age;
                }
                e->DeleteLocalRef(attacked);
            }
        }
        RegionPassCache::aq = nullptr;
        if (targetSettings.enabled && targetId >= 0 && now - targetHit < 3) {
            auto object = e->CallObjectMethod(
                level,
                jni::method(e, jni::cls(e, "net/minecraft/class_638"), "method_8469", "(I)Lnet/minecraft/class_1297;"),
                targetId);
            if (object) {
                RegionPassCache::aq = sampleEntity(e, object);
                if (RegionPassCache::aq && (RegionPassCache::aq->dead || RegionPassCache::aq->removed ||
                                            RegionPassCache::aq->spectator || RegionPassCache::aq->deathTime > 0)) {
                    RegionPassCache::aq = nullptr;
                    targetId = -1;
                }
                e->DeleteLocalRef(object);
            } else if (auto it = entities.find(targetId); it != entities.end())
                it->second->removed = true;
        }
        if (targetSettings.enabled || halo::visible())
            halo::a();
        auto meshes = [&](const Array<float> &data, int count) {
            for (int i = 0; i < count; ++i) {
                int a = i * 25;
                std::array<Point, 4> p;
                std::array<RGBA, 4> colors;
                std::array<std::array<float, 2>, 4> uv;
                for (int k = 0; k < 4; ++k) {
                    p[k] = point(data[a + k * 3], data[a + k * 3 + 1], data[a + k * 3 + 2]);
                    colors[k] = unpack(data[a + 21 + k]);
                    uv[k] = {data[a + 12 + k * 2], data[a + 13 + k * 2]};
                }
                if (canvas.mesh)
                    canvas.mesh(p, colors, uv, data[a + 20]);
                else
                    canvas.quad(p[0], p[1], p[2], p[3], colors[0]);
            }
        };
        if (canvas.layer)
            canvas.layer(false);
        meshes(RegionPassCache::QT, RegionPassCache::nQT);
        if (canvas.layer)
            canvas.layer(true);
        meshes(RegionPassCache::QA, RegionPassCache::nQA);
        auto lines = [&](const Array<float> &data, int count) {
            for (int i = 0; i < count; ++i) {
                int a = i * 11;
                canvas.line(point(data[a], data[a + 1], data[a + 2]), point(data[a + 3], data[a + 4], data[a + 5]),
                            data[a + 6], data[a + 7], unpack(data[a + 8]), unpack(data[a + 9]), int(data[a + 10]));
            }
        };
        lines(RegionPassCache::LA, RegionPassCache::nLA);
        lines(RegionPassCache::LB, RegionPassCache::nLB);
        auto sprites = [&](const Array<float> &data, int count) {
            for (int i = 0; i < count; ++i) {
                int a = i * 8;
                canvas.sprite(point(data[a], data[a + 1], data[a + 2]), data[a + 3], data[a + 4], data[a + 5],
                              static_cast<int>(data[a + 6]), unpack(data[a + 7]));
            }
        };
        sprites(RegionPassCache::SA, RegionPassCache::nSA);
        if (canvas.layer)
            canvas.layer(false);
        lines(RegionPassCache::LT, RegionPassCache::nLT);
        sprites(RegionPassCache::ST, RegionPassCache::nST);
        sprites(RegionPassCache::SB, RegionPassCache::nSB);
        lastError.clear();
    } catch (const std::exception &ex) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
        if (lastError != ex.what())
            log::write(log::Level::Error, "visuals", ex.what());
        lastError = ex.what();
        resetWorld();
    } catch (...) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
        lastError = "Native world effect failed";
        resetWorld();
    }
    activeEnv = nullptr;
    activeWorld = heightType = nullptr;
    e->PopLocalFrame(nullptr);
}
} // namespace dt::modules::bloom
