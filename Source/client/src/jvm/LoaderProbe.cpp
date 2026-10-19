#include "jvm/LoaderProbe.h"

#include "util/Log.h"

namespace vanta::jvm {

namespace {

bool probe(JNIEnv* env, const char* jvmName) {
    jclass c = env->FindClass(jvmName);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (c) { env->DeleteLocalRef(c); return true; }
    return false;
}

} // namespace

Loader detectLoader(JNIEnv* env) {
    if (!env) return Loader::Unknown;

    if (probe(env, "net/fabricmc/loader/impl/launch/knot/Knot"))          return Loader::Fabric;
    if (probe(env, "net/fabricmc/loader/impl/launch/FabricLauncherBase")) return Loader::Fabric;
    if (probe(env, "net/neoforged/fml/loading/FMLLoader"))                return Loader::NeoForge;
    if (probe(env, "net/minecraftforge/fml/loading/FMLLoader"))           return Loader::Forge;
    if (probe(env, "net/minecraft/client/main/Main"))                     return Loader::Vanilla;
    return Loader::Unknown;
}

const char* loaderName(Loader l) noexcept {
    switch (l) {
        case Loader::Vanilla:  return "Vanilla";
        case Loader::Fabric:   return "Fabric";
        case Loader::NeoForge: return "NeoForge";
        case Loader::Forge:    return "Forge";
        default:               return "Unknown";
    }
}

} // namespace vanta::jvm
