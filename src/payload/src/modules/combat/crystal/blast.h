#pragma once
// blast.h -- explosion geometry shared by Auto Crystal, Anchor Macro and
// Safe Anchor (all three live in ExplosiveModules.cpp).
//
// estimate() is the distance-only reading of the vanilla damage formula,
// pure C++, no JNI: the explosives rank candidate bases / anchor spots with
// it. Exposure is assumed 1, so the number is an upper bound -- and since it
// falls monotonically with the distance from the explosion centre to the
// victim's feet, "closest to the victim" and "most damage" are the same
// order: the ranking sorts by it and stops at the first candidate the world
// agrees with. There is no self-damage estimate any more: nothing here has a
// self-safety brake, the player decides how close to stand.
//
// The formula, from Explosion.collectBlocksAndDamageEntities (1.21) and
// ExplosionBehavior.calculateDamage (1.21.2+), identical in both:
//
//     radius   = power * 2                       (anchor 5 -> 10, crystal 6 -> 12)
//     dist     = |feet - centre| / radius        (nothing beyond 1)
//     impact   = (1 - dist) * exposure
//     raw      = (impact * impact + impact) / 2 * 7 * radius + 1
#include <algorithm>
#include <cmath>
#include "modules/shared/NativeAccess.h"

namespace dt::modules::blast {

constexpr float kAnchorPower  = 5.f;   // RespawnAnchorBlock.explode -> createExplosion(..., 5.0F, true, BLOCK)
constexpr float kCrystalPower = 6.f;   // EndCrystalEntity -> createExplosion(..., 6.0F, false, BLOCK)

// Explosion centre for an anchor at block (x,y,z): the block centre. For a
// crystal placed on base (x,y,z): the crystal's own position, i.e. the top
// face centre of the base.
inline native::Vec anchorCentre(int x,int y,int z) { return {x+.5,y+.5,z+.5}; }                         // BlockPos.toCenterPos()
inline native::Vec crystalCentre(int baseX,int baseY,int baseZ) { return {baseX+.5,baseY+1.0,baseZ+.5}; } // EndCrystalEntity.getX/getY/getZ

// Raw damage an explosion of `power` centred at `centre` deals to a victim
// standing at `feet` (Entity.squaredDistanceTo measures from the position,
// i.e. the feet, not the eye) with exposure taken as 1. 0 outside the radius.
// Pure arithmetic: safe to call for hundreds of candidates per scan.
inline double estimate(native::Vec centre,float power,native::Vec feet) {
    const double radius=double(power)*2.0;
    if(radius<=0.0) return 0.0;
    const double dist=std::sqrt(native::distance2(feet,centre))/radius;
    const double impact=std::clamp(1.0-dist,0.0,1.0);
    if(impact<=0.0) return 0.0;
    return (impact*impact+impact)/2.0*7.0*radius+1.0;
}

} // namespace dt::modules::blast
