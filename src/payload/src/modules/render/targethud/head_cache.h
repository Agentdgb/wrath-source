// head_cache.h — Minecraft heads, keyed by username.
//
// The client already fetches one head: the signed-in account's, in
// core/profile/PlayerProfile.cpp. That code is a single slot — one global pixel
// buffer, one GLuint, one name — and deliberately so, because nothing else
// needed a second. It does not generalise, and the chat's own workaround for
// that (drawing a static icon for everyone who is not you, chat.cc:515) is the
// shape of what happens when you try.
//
// So: a real cache. Keyed by name, bounded, with the two failure modes that a
// per-name fetcher must have and a single-slot one never needed — a request is
// never issued twice for the same name, and a name the services do not know is
// remembered as unknown instead of being asked for again every frame.

#pragma once

#include "imgui.h"

#include <string>

namespace dt::modules::targethud::heads {

/* @texture_for:
    the head for `username`, or 0 while it is unknown.

    Safe to call every frame with the same name — that is the expected use. The
    first call for a name queues a fetch on the worker; later calls are a map
    lookup. Must be called on the render thread: it creates the GL texture when
    the pixels land, and a GL object may only be made on the thread owning the
    context. */
ImTextureID texture_for(const std::string& username);

/* Stops the worker and deletes every texture. Called from the gui shutdown,
   which is the only place the GL context is still current. */
void shutdown();

} // namespace dt::modules::targethud::heads
