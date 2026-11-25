// item_shader_log.h — the ItemShader feature's OWN desktop debug channel.
//
// Writes to <Desktop>\gl_shader_trace.txt, separate from gl_entity_trace.txt so the
// first-person item-shader bring-up does not interleave with the chams spam and
// can be read on its own. Shared by the two translation units that implement the
// feature: chams_gl.cpp (the GL-layer observe path + shader dump) and
// animations_hook.cpp (the renderFirstPersonItem detour that raises the flag).
//
// desklog::open() is idempotent per basename, so each TU resolving its own local
// static still lands on the exact same channel/file.

#pragma once

#include "util/DeskLog.h"

namespace dt::modules::itemshader {

inline ::dt::desklog::Channel log_ch() {
    static ::dt::desklog::Channel ch = ::dt::desklog::open("gl_shader_trace");
    return ch;
}

} // namespace dt::modules::itemshader

// printf-style, one line, flushed immediately.
#define ITEMSHADER_LOG(...) \
    ::dt::desklog::logf(::dt::modules::itemshader::log_ch(), __VA_ARGS__)

// Rate-limited by call site (collision-proof; see VDESK_RL_SITE in DeskLog.h).
#define ITEMSHADER_RL(ms, ...) \
    VDESK_RL_SITE(::dt::modules::itemshader::log_ch(), (ms), __VA_ARGS__)

