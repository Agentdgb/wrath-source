#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Value-like array handles preserve Java's reference/alias semantics. Checked
// indexing also preserves its bounds failures instead of corrupting native memory.
namespace vanta::modules::bloom {
template <class T> class Array {
    std::shared_ptr<std::vector<T>> values_;

  public:
    Array() = default;
    Array(std::nullptr_t) {}
    explicit Array(int n) : values_(std::make_shared<std::vector<T>>(checkedSize(n))) {}
    Array(std::initializer_list<T> values) : values_(std::make_shared<std::vector<T>>(values)) {}
    static std::size_t checkedSize(int n) {
        if (n < 0 || n > 16777216)
            throw std::out_of_range("Bloom array size");
        return n;
    }
    typename std::vector<T>::reference operator[](int n) {
        if (!values_ || n < 0 || static_cast<std::size_t>(n) >= values_->size())
            throw std::out_of_range("Bloom array index " + std::to_string(n) + "/" + std::to_string(size()));
        return values_->at(n);
    }
    typename std::vector<T>::const_reference operator[](int n) const {
        if (!values_ || n < 0 || static_cast<std::size_t>(n) >= values_->size())
            throw std::out_of_range("Bloom array index " + std::to_string(n) + "/" + std::to_string(size()));
        return values_->at(n);
    }
    std::size_t size() const { return values_ ? values_->size() : 0; }
    bool operator==(std::nullptr_t) const { return !values_; }
    bool operator!=(std::nullptr_t) const { return !!values_; }
    void fill(T v) {
        if (values_)
            std::fill(values_->begin(), values_->end(), v);
    }
    auto begin() {
        if (!values_)
            throw std::runtime_error("Null Bloom array");
        return values_->begin();
    }
    auto end() {
        if (!values_)
            throw std::runtime_error("Null Bloom array");
        return values_->end();
    }
    auto begin() const {
        if (!values_)
            throw std::runtime_error("Null Bloom array");
        return values_->begin();
    }
    auto end() const {
        if (!values_)
            throw std::runtime_error("Null Bloom array");
        return values_->end();
    }
};
struct Math {
    static constexpr double PI = 3.14159265358979323846;
    template <class T, class U> static auto min(T a, U b) {
        using R = decltype(a + b);
        return std::min<R>(a, b);
    }
    template <class T, class U> static auto max(T a, U b) {
        using R = decltype(a + b);
        return std::max<R>(a, b);
    }
    template <class T> static auto abs(T x) { return std::abs(x); }
    static double log10(double x) { return std::log10(x); }
    static double log(double x) { return std::log(x); }
    static double acos(double x) { return std::acos(x); }
    static double asin(double x) { return std::asin(x); }
    static double ceil(double x) { return std::ceil(x); }
    template <class T> static T floorMod(T a, T b) {
        T v = a % b;
        return v < 0 ? v + b : v;
    }
    static double sqrt(double x) { return std::sqrt(x); }
    static double exp(double x) { return std::exp(x); }
    static double sin(double x) { return std::sin(x); }
    static double cos(double x) { return std::cos(x); }
    static double tan(double x) { return std::tan(x); }
    static double atan(double x) { return std::atan(x); }
    static double atan2(double x, double y) { return std::atan2(x, y); }
    static double pow(double x, double y) { return std::pow(x, y); }
    static double toRadians(double x) { return x * PI / 180; }
    static double floor(double x) { return std::floor(x); }
    static int round(float x) { return static_cast<int>(std::floor(x + .5f)); }
};
struct MathHelper {
    static int hsvToRgb(float h, float s, float v) {
        int sector = int(h * 6) % 6;
        float f = h * 6 - std::floor(h * 6), p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
        float r, g, b;
        switch (sector) {
        case 0:
            r = v;
            g = t;
            b = p;
            break;
        case 1:
            r = q;
            g = v;
            b = p;
            break;
        case 2:
            r = p;
            g = v;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = v;
            break;
        case 4:
            r = t;
            g = p;
            b = v;
            break;
        default:
            r = v;
            g = p;
            b = q;
        }
        return int(r * 255) << 16 | int(g * 255) << 8 | int(b * 255);
    }
    static float sin(double x) { return static_cast<float>(std::sin(x)); }
    static float cos(double x) { return static_cast<float>(std::cos(x)); }
    static float sqrt(float x) { return std::sqrt(x); }
    static int floor(double x) { return static_cast<int>(std::floor(x)); }
    template <class T, class U, class V> static auto lerp(T t, U a, V b) { return a + t * (b - a); }
    static float wrapDegrees(float x) {
        float v = std::fmod(x, 360.f);
        if (v >= 180)
            v -= 360;
        if (v < -180)
            v += 360;
        return v;
    }
    static float lerpAngleDegrees(float t, float a, float b) { return a + t * wrapDegrees(b - a); }
    template <class T> static T clamp(T x, T lo, T hi) { return std::clamp(x, lo, hi); }
};
struct Float {
    static float intBitsToFloat(std::uint32_t x) { return std::bit_cast<float>(x); }
    static std::uint32_t floatToRawIntBits(float x) { return std::bit_cast<std::uint32_t>(x); }
};
template <class T> Array<Array<T>> matrix(int rows, int columns) {
    Array<Array<T>> result(rows);
    for (int i = 0; i < rows; ++i)
        result[i] = Array<T>(columns);
    return result;
}
struct Arrays {
    template <class T> static Array<T> copyOf(const Array<T> &src, int size) {
        Array<T> out(size);
        for (int i = 0; i < std::min<int>(size, src.size()); ++i)
            out[i] = src[i];
        return out;
    }
    template <class T, class U> static void fill(Array<T> &a, int from, int to, U v) {
        for (int i = from; i < to; ++i)
            a[i] = static_cast<T>(v);
    }

    template <class T, class U> static void fill(Array<T> &a, U value) { a.fill(static_cast<T>(value)); }
};
struct Random {
    std::shared_ptr<bool> gaussianReady = std::make_shared<bool>(false);
    std::shared_ptr<double> gaussian = std::make_shared<double>(0);
    double nextGaussian() {
        if (*gaussianReady) {
            *gaussianReady = false;
            return *gaussian;
        }
        double x, y, s;
        do {
            x = 2 * nextDouble() - 1;
            y = 2 * nextDouble() - 1;
            s = x * x + y * y;
        } while (s >= 1 || s == 0);
        double m = std::sqrt(-2 * std::log(s) / s);
        *gaussian = y * m;
        *gaussianReady = true;
        return x * m;
    }
    std::shared_ptr<std::uint64_t> seed = std::make_shared<std::uint64_t>(0);
    explicit Random(
        std::uint64_t s = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {
        setSeed(s);
    }
    void setSeed(std::uint64_t s) {
        *gaussianReady = false;
        *seed = (s ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1);
    }
    std::uint32_t next(int bits) {
        *seed = (*seed * 0x5DEECE66DULL + 11) & ((1ULL << 48) - 1);
        return static_cast<std::uint32_t>(*seed >> (48 - bits));
    }
    std::int64_t nextLong() {
        auto hi = std::uint64_t(next(32)) << 32;
        auto lo = static_cast<std::int32_t>(next(32));
        return std::bit_cast<std::int64_t>(hi + static_cast<std::uint64_t>(lo));
    }
    float nextFloat() { return next(24) * 0x1p-24f; }
    double nextDouble() { return ((std::uint64_t(next(26)) << 27) + next(27)) * 0x1p-53; }
    bool nextBoolean() { return next(1) != 0; }
    int nextInt() { return std::bit_cast<std::int32_t>(next(32)); }
    int nextInt(int bound) {
        if (bound <= 0)
            throw std::out_of_range("Random bound");
        if ((bound & -bound) == bound)
            return static_cast<int>((std::int64_t(bound) * next(31)) >> 31);
        for (;;) {
            auto bits = next(31);
            int value = bits % bound;
            if (static_cast<std::int32_t>(bits - value + (bound - 1)) >= 0)
                return value;
        }
    }
};
inline Random &threadRandom() {
    static thread_local Random r;
    return r;
}
template <class T> void arraycopy(const Array<T> &src, int start, Array<T> &dst, int offset, int count) {
    if (count < 0 || start < 0 || offset < 0 || std::size_t(start + count) > src.size() ||
        std::size_t(offset + count) > dst.size())
        throw std::out_of_range("Bloom arraycopy");
    std::vector<T> temp;
    for (int i = 0; i < count; ++i)
        temp.push_back(src[start + i]);
    for (int i = 0; i < count; ++i)
        dst[offset + i] = temp[i];
}
inline int bitCount(int value) { return std::popcount(static_cast<unsigned>(value)); }
using Integer = int;
enum class CameraSubmersionType { NONE, WATER, LAVA, POWDER_SNOW };
struct StatusEffects {
    static constexpr int BLINDNESS = 0, DARKNESS = 1, NAUSEA = 2;
};
struct Heightmap {
    struct Type {
        static constexpr int MOTION_BLOCKING_NO_LEAVES = 0;
    };
};
enum class EntityPose { STANDING, CROUCHING, OTHER };
struct EquipmentSlot {
    static constexpr int HEAD = 0, CHEST = 1;
};
struct ItemStack {
    bool empty = true;
    bool isEmpty() const { return empty; }
};
struct GameProfile {
    std::string value;
    const std::string &name() const { return value; }
};
struct EntityView {
    double x = 0, y = 0, z = 0, lastRenderX = 0, lastRenderY = 0, lastRenderZ = 0;
    float height = 1.8f, width = .6f, bodyYaw = 0, lastBodyYaw = 0, headYaw = 0, lastHeadYaw = 0, pitch = 0, scale = 1,
          nauseaIntensity = 0, lastNauseaIntensity = 0;
    int id = 0, hurtTime = 0, deathTime = 0, maxHurtTime = 10;
    bool player = false, dead = false, removed = false, spectator = false, sleeping = false, sneaking = false;
    std::array<bool, 3> effects{};
    std::array<bool, 2> equipment{};
    GameProfile profile;
    int getId() const { return id; }
    float getFovMultiplier(bool, float) const { return 1; }
    float getDamageTiltYaw() const { return 0; }
    bool hasStatusEffect(int e) const { return effects.at(e); }
    double getX() const { return x; }
    double getY() const { return y; }
    double getZ() const { return z; }
    float getHeight() const { return height; }
    float getWidth() const { return width; }
    float getPitch(float) const { return pitch; }
    float getScale() const { return scale; }
    bool isRemoved() const { return removed; }
    bool isSpectator() const { return spectator; }
    bool isDead() const { return dead; }
    bool isSleeping() const { return sleeping; }
    bool isInSneakingPose() const { return sneaking; }
    EntityPose getPose() const { return sneaking ? EntityPose::CROUCHING : EntityPose::STANDING; }
    EntityView *getVehicle() const { return nullptr; }
    ItemStack getEquippedStack(int slot) const { return {!equipment.at(slot)}; }
    const GameProfile &getGameProfile() const { return profile; }
    double squaredDistanceTo(double a, double b, double c) const {
        return (x - a) * (x - a) + (y - b) * (y - b) + (z - c) * (z - c);
    }
};
using LivingEntity = EntityView *;
using Entity = EntityView *;
using PlayerEntity = EntityView *;
using AbstractClientPlayerEntity = EntityView *;
struct List {
    std::vector<EntityView *> items;
    int size() const { return static_cast<int>(items.size()); }
    EntityView *get(int i) const { return items.at(i); }
};
struct BindingBridge {
    bool d() const { return true; }
    bool c(const std::string &) const { return false; }
};
struct Configuration {
    BindingBridge *e() const { return nullptr; }
};
struct ConfigBridge {
    static inline Configuration *c = nullptr;
};
struct NativeWorld {
    bool (*visible)(double, double, double) = nullptr;
    double (*trace)(double, double, double, double) = nullptr;
    void (*sound)(int, double, double, double) = nullptr;
    int bottom = -64;
    int (*height)(int, int) = nullptr;
    List players;
    List *getPlayers() { return &players; }
    int getBottomY() const { return bottom; }
    int getTopY(int, int x, int z) const { return height ? height(x, z) : bottom; }
};
using ClientWorld = NativeWorld *;
struct Camera {
    CameraSubmersionType submersion = CameraSubmersionType::NONE;
    EntityView *focused = nullptr;
    CameraSubmersionType getSubmersionType() const { return submersion; }
    EntityView *getFocusedEntity() const { return focused; }
};
struct GameRenderer {
    Camera camera;
    Camera *getCamera() { return &camera; }
};
struct Window {
    int width = 1, height = 1;
    int getFramebufferWidth() const { return width; }
    int getFramebufferHeight() const { return height; }
};
struct Fov {
    int value = 90;
    int getValue() const { return value; }
};
struct Perspective {
    bool first = true;
    bool isFirstPerson() const { return first; }
};
struct DoubleOption {
    float value = 1;
    float getValue() const { return value; }
};
struct Options {
    DoubleOption effect, damage;
    DoubleOption &getFovEffectScale() { return effect; }
    DoubleOption &getDamageTiltStrength() { return damage; }
    Fov fov;
    Perspective perspective;
    Fov &getFov() { return fov; }
    Perspective &getPerspective() { return perspective; }
};
struct MinecraftClient {
    ClientWorld world = nullptr;
    EntityView *player = nullptr;
    GameRenderer gameRenderer;
    Window window;
    Options options;
    Window &getWindow() { return window; }
    EntityView *getCameraEntity() const { return gameRenderer.camera.focused; }
    static MinecraftClient *getInstance();
};
struct RegionPassCache {
    static inline double z = 0, aa = 0, ab = 0, pvX = 0, pvY = 0, pvZ = 0, now = 0;
    static inline float dt = 0, fX = 0, fY = 0, fZ = -1, rX = 1, rY = 0, rZ = 0, uX = 0, uY = 1, uZ = 0, bn = 1;
    static inline float ac = 540, td = 1, ae = 1, am = 1, an = 1, ao = .7f, ap = 1, au = 1, av = 1, pvH = 1.8f, bp = 0,
                        bq = 0, br = 0, bs = 0;
    static inline bool ah = false, ai = true, ar = false;
    static inline int af = 0xC080FF, ag = 0xFFFFFF, aj = 4, ak = 0, al = -3, as = 0, at = -3, nQA = 0, nQS = 0;
    static inline LivingEntity aq = nullptr;
    static inline bool bh = false, bi = true, bj = false, bk = false, bl = false, bm = false, PV = false;
    static inline int bo = -3, bt = 0, nLA = 0, nQT = 0, nSA = 0, nST = 0;
    static inline Array<float> LA = Array<float>(8192 * 11), QT = Array<float>(4096 * 25), SA = Array<float>(4096 * 8),
                               ST = Array<float>(1024 * 8);
    static inline Array<float> QA = Array<float>(4096 * 25), QS = Array<float>(4096 * 25);
    static inline int nLT = 0, nLB = 0, nSB = 0;
    static inline Array<float> LT = Array<float>(512 * 11), LB = Array<float>(512 * 11), SB = Array<float>(512 * 8);
    static bool l(LivingEntity entity, bool) { return entity && !entity->removed && !entity->spectator; }
    static int col(int palette);
    static void clear() { nLA = nQT = nSA = nST = nQA = nQS = nLT = nLB = nSB = 0; }
};
namespace world {
void a();
void b();
void c();
void d();
bool visible();
} // namespace world
namespace halo {
void a();
void p();
bool visible();
} // namespace halo
} // namespace vanta::modules::bloom
