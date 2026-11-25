#pragma once
namespace dt::hook::packets {
void install(); // render thread, after runtime detection
bool shutdown(); // stop listener dispatch, drain, detach, then drain late entries
bool ready();
bool unavailable();
}
