#pragma once
#ifndef WRATH_STRINGS_MC_NAMES_H
#define WRATH_STRINGS_MC_NAMES_H

#include "core/enc.h"

namespace wrath::strings {

// Minecraft Classes (JNI internal form)
constexpr auto kEntityPlayer               = "net/minecraft/world/entity/player/Player"_e;
constexpr auto kMinecraftClient            = "net/minecraft/client/Minecraft"_e;
constexpr auto kLocalPlayer                = "net/minecraft/client/player/LocalPlayer"_e;
constexpr auto kClientLevel                = "net/minecraft/client/multiplayer/ClientLevel"_e;
constexpr auto kEntity                     = "net/minecraft/world/entity/Entity"_e;
constexpr auto kHitResult                  = "net/minecraft/world/phys/HitResult"_e;
constexpr auto kBlockHitResult             = "net/minecraft/world/phys/BlockHitResult"_e;
constexpr auto kEntityHitResult            = "net/minecraft/world/phys/EntityHitResult"_e;
constexpr auto kVec3                       = "net/minecraft/world/phys/Vec3"_e;
constexpr auto kAABB                       = "net/minecraft/world/phys/AABB"_e;
constexpr auto kItemStack                  = "net/minecraft/world/item/ItemStack"_e;

// Fields & Coordinates
constexpr auto kPosX                       = "x"_e;
constexpr auto kPosY                       = "y"_e;
constexpr auto kPosZ                       = "z"_e;
constexpr auto kYaw                        = "yRot"_e;
constexpr auto kPitch                      = "xRot"_e;
constexpr auto kOnGround                   = "onGround"_e;

// JVM & JNI Export Names (hashed with _h, but decrypted if needed)
constexpr auto kJniGetCreatedJavaVMs       = "JNI_GetCreatedJavaVMs"_e;
constexpr auto kJvmDll                     = "jvm.dll"_e;

} // namespace wrath::strings

#endif // WRATH_STRINGS_MC_NAMES_H
