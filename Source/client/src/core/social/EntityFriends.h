#pragma once

// "Is this entity somebody the user marked as a friend?"
//
// The one question the modules ask. It lives in core rather than in the GUI
// because the answer is needed on the game thread, inside targeting loops and
// inside a JNIHook detour, and src/vgui is neither reachable nor safe from
// there.
//
// COST, and why there is no cache.
//
// The first design cached the answer per entity id behind a mutex, holding weak
// global refs to validate identity. That bought very little and cost a great
// deal: the lock was taken while making JNI calls, from three thread domains at
// once (the game thread, AimAssist's own worker, and a JNIHook detour running
// inside game code), and entity ids are reused across worlds so the cache
// needed an invalidation hook that does not exist in this tree.
//
// What is actually paid, uncached, is four JNI calls and no allocation -- and
// only when BOTH of these hold:
//
//   * the user has at least one friend. FriendList::empty() is a relaxed atomic
//     load, so for everybody else this is free.
//   * the entity already survived that call site's own filters. Every caller
//     asks late: after the class test, after distance, after angle. Typically
//     nought to three entities per query reach here, not the whole world.
//
// So the honest version is cheaper in the case that matters and cannot go
// stale. If a profiler ever disagrees, cache in the CALLER, where the lifetime
// of an entity reference is already understood.

#include <jni.h>

namespace vanta::social {

// False for a non-player, for an unresolved build, and whenever the friend list
// is empty. Never throws and never leaves a pending Java exception behind.
bool isFriend(JNIEnv* env, jobject entity);

} // namespace vanta::social
