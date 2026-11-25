#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dt::profile {

// The signed-in player, as shown on the sidebar card.
//
// Two independent pieces that resolve at different times and must not block
// each other: the NAME comes from the JVM and is available as soon as the
// client has attached, in the menu as well as in-world; the HEAD is a small
// PNG fetched over HTTPS and may never arrive at all (offline, blocked, no
// such account). The gui shows whatever it has.

// Kick off resolution. Returns immediately; everything happens on a worker.
void start();

// Stop the worker and release the pixels. Safe to call without start().
void stop();

// The account name, or an empty string until the JVM has been asked.
std::string name();

// True once the head PNG has been decoded and is waiting to be uploaded.
// Consumed by the gui, which owns the GL context and does the upload.
bool hasPendingHead();

// Hands over the decoded RGBA pixels exactly once; leaves the buffer empty.
// Returns false when there is nothing pending.
bool takeHead(std::vector<std::uint8_t>& rgba, int& width, int& height);

} // namespace dt::profile
