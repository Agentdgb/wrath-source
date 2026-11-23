#include "modules/combat/aimassist/simple/Engine.h"
#include "modules/combat/aimassist/wind/aimassist.h"
#include "modules/combat/aimassist/wind/Engine.h"
#include "modules/combat/aimassist/wind/aimassist_debug.h"
#include "util/Log.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
namespace {
JNIEnv *activeEnv = nullptr;
bool friendFilter = false;
} // namespace
namespace vanta::jvm {
JniScope::JniScope() : env_(activeEnv) {}
JniScope::~JniScope() = default;
jclass findClassAnyLoader(JNIEnv *env, const char *name) { return env->FindClass(name); }
std::string toStdString(JNIEnv *env, jstring value) {
    if (!value)
        return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    if (chars)
        env->ReleaseStringUTFChars(value, chars);
    return result;
}
} // namespace vanta::jvm
namespace vanta::vgui::bridge { bool isOverlayVisible(){return false;} bool isEnabled(const std::string&){return false;} }
namespace vanta::log { void write(Level, std::string_view, std::string) noexcept {} }
namespace vanta::modules::aimassist::debug {
void setEnabled(bool) {} bool active(){return false;}
void workerStage(const char*) {} void renderStage(const char*) {} void frameStatus(const char*,bool,bool) {}
void sessionOpen(int,float,float,bool,bool,float) {} void sessionClose() {} void resolveLine(const char*) {}
void workerTick(double) {} void opts(int,float,float,bool,bool) {} void optsTick() {} void setQuantum(float) {}
void emit(float,float,float,float,float,float,float,float) {} void ctrlAdaptive(const AdaptiveSnap&) {}
}
namespace vanta::jvm { JvmContext& ctx() noexcept { static JvmContext value; return value; } }
namespace vanta::social {
bool isFriend(JNIEnv *, jobject) { return friendFilter; }
bool isTeammate(JNIEnv *, jobject, jobject) { return false; }
} // namespace vanta::social
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    JavaVM *vm = nullptr;
    try {
        const std::filesystem::path path{WRATH_TEST_JVM_DLL};
        const auto bin = path.parent_path().parent_path();
        SetDllDirectoryW(bin.c_str());
        LoadLibraryW((bin / L"jli.dll").c_str());
        auto library = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        require(library != nullptr, "Cannot load JVM");
        using Create = jint(JNICALL *)(JavaVM **, void **, void *);
        auto create = reinterpret_cast<Create>(GetProcAddress(library, "JNI_CreateJavaVM"));
        std::string classpath = std::string("-Djava.class.path=") + WRATH_TEST_CLASSPATH;
        JavaVMOption options[] = {{classpath.data(), nullptr}};
        JavaVMInitArgs args{JNI_VERSION_1_8, 1, options, JNI_FALSE};
        require(create && create(&vm, reinterpret_cast<void **>(&activeEnv), &args) == JNI_OK, "Cannot create JVM");
        auto env = activeEnv;
        auto fixture = env->FindClass("net/minecraft/AimFixture");
        require(fixture != nullptr, "Fixture missing");
        auto reset = env->GetStaticMethodID(fixture, "reset", "()V");
        auto set = env->GetStaticMethodID(fixture, "set", "(ID)V");
        auto get = env->GetStaticMethodID(fixture, "get", "(I)D");
        auto target = env->GetStaticMethodID(fixture, "target", "()Ljava/lang/Object;");
        require(reset && set && get && target, "Fixture API missing");
        auto put = [&](int field, double value) {
            env->CallStaticVoidMethod(fixture, set, field, value);
            require(!env->ExceptionCheck(), "Fixture setter failed");
        };
        auto read = [&](int field) { return env->CallStaticDoubleMethod(fixture, get, field); };
        {
            using namespace vanta::modules::aimassist;
            env->CallStaticVoidMethod(fixture, reset);
            require(ensure_cache(env), "OG legacy JNI cache unavailable");
            const auto& c=cache();
            require(c.mc_instance_fid && c.mc_player_fid && c.ent_yaw_fid && c.ent_pitch_fid,
                    "OG required fields missing from real JVM fixture");
            SharedState shared;
            SettingsView sv;
            sv.requireMouseDown=false;sv.horizontalSpeed=15;sv.verticalSpeed=10;
            sv.aimVertically=true;sv.targetArea=1;sv.maxAngle=180;sv.distance=5;
            sv.emissionSmooth=5;sv.jumpCompensate=false;sv.humanize=false;
            int cooldown=0;
            auto mc=env->GetStaticObjectField(c.minecraft_class,c.mc_instance_fid);
            auto player=env->GetObjectField(mc,c.mc_player_fid);
            for(int i=0;i<300;++i) {
                regular::workerTick(env,shared,sv,cooldown);
                const float yaw=env->GetFloatField(player,c.ent_yaw_fid);
                const float previous=c.ent_prev_yaw_fid?env->GetFloatField(player,c.ent_prev_yaw_fid):0;
                regular::renderApply(env,player,shared,sv);
                if(c.ent_prev_yaw_fid) require(std::abs((env->GetFloatField(player,c.ent_prev_yaw_fid)-previous)-
                    (env->GetFloatField(player,c.ent_yaw_fid)-yaw))<.0001,
                    "OG interpolation does not follow the applied camera delta");
                Sleep(1);
            }
            require(shared.targetRef && read(1)<-65, "Unmodified OG Regular does not acquire and rotate");
            regular::onDisable(env,shared);
            put(2,-40);
            vanta::jvm::ctx().vm.store(vm);
            Runtime threaded;
            threaded.settings=sv;
            threaded.moduleEnabled.store(true);
            threaded.start();
            for(int i=0;i<80;++i) {
                Sleep(4);
                std::lock_guard control(threaded.control);
                regular::renderApply(env,player,threaded.shared,threaded.settings);
            }
            threaded.stop();
            require(!threaded.running && !threaded.worker.joinable() && threaded.shared.targetRef && read(1)<-65,
                    "OG worker lifecycle does not acquire, apply and stop correctly");
            regular::onDisable(env,threaded.shared);
            require(!threaded.shared.targetRef,"OG worker cleanup retains target");
            {
                put(2,-40);
                wind::Engine isolatedWind;
                for(int i=0;i<80;++i) { isolatedWind.step(env,sv); Sleep(4); }
                require(std::string_view(isolatedWind.status())=="Tracking" && read(1)<-65,
                        "Separated Wind engine does not follow using its own runtime");
                isolatedWind.pause();
                require(std::string_view(isolatedWind.status())=="Paused", "Separated Wind does not pause");
                put(2,-40);
                simple::Engine isolatedSimple;
                readableaim::Settings simpleSettings;
                for(int i=0;i<100;++i) isolatedSimple.step(env,simpleSettings,1.+i/144.);
                require(isolatedSimple.target() && read(1)<-65,
                        "Simple cannot follow after stopping Wind");
                isolatedSimple.clear(env);
                isolatedSimple.pause("Wind active");
                put(2,-40);
                for(int i=0;i<80;++i) { isolatedWind.step(env,sv); Sleep(4); }
                require(std::string_view(isolatedWind.status())=="Tracking" && read(1)<-65,
                        "Wind cannot resume after running Simple independently");
                isolatedWind.pause();
            }
            require(!shared.targetRef, "OG disable retains target reference");
            env->DeleteLocalRef(player);env->DeleteLocalRef(mc);
            drop_cache(env);
        }
        {
            using namespace readableaim;
            vanta::modules::aimassist::simple::Engine game;
            Settings settings;
            double time = 1;
            auto start = [&] {
                game.clear(env);
                env->CallStaticVoidMethod(fixture, reset);
                friendFilter = false;
                settings = {};
            };
            auto frames = [&](int count, bool focus = false) {
                for (int i = 0; i < count; ++i) {
                    time += 1. / 144;
                    game.step(env, settings, time, focus);
                }
                if (!game.error().empty())
                    throw std::runtime_error(game.error());
            };
            auto rival = [&] {
                auto object = env->CallStaticObjectMethod(fixture, target);
                bool same = env->IsSameObject(game.target(), object);
                env->DeleteLocalRef(object);
                return same;
            };
            start();
            settings.position = AimPosition::Random;
            settings.prediction = 0;
            settings.vertical = true;
            put(2, -80);
            frames(400);
            require(rival(), "Random tracking did not acquire a rival");
            const double stableYaw = read(1), stablePitch = read(2);
            frames(100);
            require(std::abs(read(1) - stableYaw) < .001 && std::abs(read(2) - stablePitch) < .001,
                    "Random aim point changes between attacks");
            for (int attack = 0; attack < 5; ++attack) {
                put(28, 12);
                frames(1);
                put(28, 0);
                frames(300);
                require(read(1) > -93 && read(1) < -87 && read(2) > 0 && read(2) < 20,
                        "Random attack point moves to a hitbox edge");
            }
            for (double sensitivity : {0., .25, .5, 1.}) {
                start();
                put(26, sensitivity);
                frames(2);
                const double before = read(1);
                frames(1);
                const double step = mouseAngularStep(sensitivity);
                const double counts = (read(1)-before)/step;
                require(read(1) != before && std::abs(counts-std::round(counts)) < .0005,
                        "Native output ignores the real Minecraft sensitivity option");
            }
            start();
            put(25, 0);
            frames(10);
            require(rival() && read(3) > 0, "Assistance still requires holding attack");
            frames(100);
            require(rival() && read(1) < -70 && read(2) == 0 && read(3) > 0,
                    "New JNI path does not acquire and rotate");
            frames(15);
            require(rival(), "Auto Focus loses an eligible rival");
            put(1, 50);
            put(18, 20);
            const double retained = read(1);
            frames(20);
            require(rival() && read(1) > retained, "Focus range does not extend tracking");
            settings.maxFocusDistance = 10;
            put(18, 30);
            const double outsideYaw = read(1);
            frames(20);
            require(rival() && read(1) > outsideYaw, "Auto Focus stops tracking beyond the old range");
            settings.maxFocusDistance = 100;
            put(1, 3);
            put(18, 0);
            frames(20);
            require(rival(), "Returning rival is not reacquired");
            settings.fov = 180;
            put(2, read(1) + 25);
            const double manual = read(1);
            frames(40);
            require(read(1) == manual && std::string_view(game.status()) == "Manual aim",
                    "Camera fights deliberate mouse departure");
            put(2, manual - 2);
            frames(40);
            require(read(1) < manual - 2, "Turning back does not resume");
            start();
            frames(40);
            require(rival(), "Auto Focus did not acquire the first rival");
            put(5, 1);
            put(1, 50);
            frames(20);
            require(rival(), "Auto Focus replaced a valid retained rival");
            const auto otherMethod = env->GetStaticMethodID(fixture, "otherTarget", "()Ljava/lang/Object;");
            const auto other = env->CallStaticObjectMethod(fixture, otherMethod);
            game.focusAttack(env, other, settings);
            frames(10);
            require(other && env->IsSameObject(game.target(), other), "Attacking a different target does not switch Focus");
            env->DeleteLocalRef(other);
            start();
            frames(40);
            put(2, -40);
            const double beforePause = read(3);
            game.pause("Host overlay");
            time += 1;
            game.step(env, settings, time, true);
            require(read(3) == beforePause && read(1) == -40, "Host pause resumes with a stale large frame interval");
            frames(40);
            require(rival() && read(1) < -40, "Host pause never resumes tracking");
            start();
            frames(40);
            require(rival(), "Assistance stops without attack input");
            start();
            settings.fov = 20;
            frames(10);
            require(!game.target(), "Acquisition ignores Fov");
            put(2, -80);
            frames(20);
            require(rival(), "Entering Fov does not acquire");
            start();
            put(1, 50);
            frames(10);
            require(!game.target(), "Focus range widens initial acquisition");
            start();
            settings.focusMode = FocusMode::Keybind;
            frames(30, true);
            put(5, 1);
            put(2, -40);
            frames(1, true);
            require(rival(), "Held focus key does not retain rival");
            frames(1, false);
            require(!rival() && game.target(), "Releasing focus key does not permit a new target");
            start();
            put(5, 1);
            settings.focusMode = FocusMode::Keybind;
            frames(2);
            require(!rival(), "Mouse priority does not prefer angular distance");
            settings.priority = Priority::Distance;
            frames(2);
            require(rival(), "Distance priority does not prefer nearest entity");
            start();
            settings.onlyWeapon = true;
            put(7, 0);
            frames(10);
            require(!game.target() && std::string_view(game.status()) == "Weapon required",
                    "Weapon gate accepts empty hand");
            for (int weapon = 1; weapon <= 5; ++weapon) {
                put(7, weapon);
                frames(20);
                require(rival(), "Weapon tags, mace or trident not supported");
                frames(1);
            }
            start();
            frames(20);
            double held = read(1);
            put(6, 1);
            frames(20);
            require(read(1) == held, "Game screen does not pause assistance");
            put(6, 0);
            frames(20);
            require(read(3) > 0 && rival(), "Closing the game screen does not resume assistance");
            start();
            frames(20);
            require(rival(), "Initial target not acquired");
            put(29, 1);
            const double yawWhileEating = read(1);
            frames(20);
            require(read(1) == yawWhileEating && std::string_view(game.status()) == "Eating / using item",
                    "Eating food/gap does not pause AimAssist");
            require(rival(), "Eating drops acquired rival");
            put(29, 0);
            frames(20);
            require(rival() && read(1) < yawWhileEating, "Stopping eating does not resume tracking rival");
            start();
            settings.distance = 350;
            settings.maxFocusDistance = 350;
            put(1, 340);
            frames(30);
            require(rival(), "Rival at 340 blocks was not acquired with distance=350");
            put(22, 1);
            held = read(1);
            frames(20);
            require(read(1) == held, "Paused game rotates camera");
            start();
            put(20, 1);
            frames(10);
            require(!game.target(), "Line-of-sight gate ignores walls");
            put(20, 0);
            frames(30);
            require(rival(), "Restored line of sight does not resume assistance");
            start();
            settings.fov = 180;
            settings.vertical = true;
            put(21, 1);
            frames(10);
            require(!game.target(), "Opaque initial line of sight is accepted");
            frames(40);
            require(!game.target() && read(4) == 0, "Removed half-block scanning is still active");
            start();
            settings.fov = 180;
            settings.vertical = true;
            put(3, -30);
            frames(80);
            require(read(2) > -20, "Automatic body point does not correct vertical aim");
            start();
            settings.fov = 180;
            settings.position = AimPosition::Random;
            frames(80);
            require(rival() && read(2) == 0, "Random position breaks acquisition or disables vertical gate");
            start();
            settings.prediction = 0;
            settings.fov = 180;
            put(24, 1);
            frames(80);
            double noPrediction = read(1);
            start();
            settings.prediction = .091;
            settings.fov = 180;
            put(24, -1);
            frames(80);
            require(read(1) < noPrediction - 2 && read(1) > -97 && read(1) < -83,
                    "Prediction must follow velocity while staying inside the target hitbox");
            for (int kind = 1; kind <= 4; ++kind) {
                start();
                put(19, kind);
                frames(10);
                require(!game.target(), "Disabled entity category is acquired");
                settings.players = false;
                settings.mobs = kind == 1;
                settings.animals = kind == 2;
                settings.villagers = kind == 3;
                settings.crystals = kind == 4;
                frames(40);
                require(rival(), "Enabled entity category is not acquired");
            }
            start();
            put(19, 5);
            settings.mobs = true;
            frames(10);
            require(!game.target(), "Armor stands are incorrectly classified as mobs");
            start();
            friendFilter = true;
            frames(10);
            require(!game.target(), "Native client friend protection is lost");
            friendFilter = false;
            frames(20);
            require(rival(), "Friend filter does not recover");
            put(11, 1);
            frames(5);
            require(rival() && std::string_view(game.status()) == "Target retained outside assistance",
                    "Focus loses a temporarily invisible rival");
            start();
            frames(20);
            put(4, 0);
            frames(5);
            require(!game.target(), "Dead rival is retained");
            start();
            frames(20);
            put(9, 1);
            frames(5);
            require(!game.target(), "World transition retains stale global reference");
            start();
            frames(20);
            put(13, 0);
            frames(5);
            require(!game.target(), "Disconnect retains target");
            start();
            settings.mode = AimMode::Wind;
            frames(50);
            require(rival() && read(3) > 0, "Wind cannot drive the JNI path");
            start();
            settings.mode = AimMode::Wind;
            put(17, 1);
            for (int i = 0; i < 30; ++i) {
                time += 1. / 144;
                game.step(env, settings, time, true);
            }
            require(!game.error().empty() && std::string_view(game.status()) == "Compatibility error",
                    "Native rotation failure is hidden");
            put(17, 0);
            frames(100);
            require(game.error().empty() && rival(), "JNI path does not recover after rotation failure");
        }
        env->DeleteLocalRef(fixture);
        vm->DestroyJavaVM();
        activeEnv = nullptr;
        std::cout << "PASS: new AimAssist JNI integration, targeting, settings, focus and recovery\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (vm)
            vm->DestroyJavaVM();
        return 1;
    }
}
