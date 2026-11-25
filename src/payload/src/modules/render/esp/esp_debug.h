// esp_debug.h — desktop file logger for bringing up the Player ESP module.
// Writes to %USERPROFILE%\Desktop\dt_esp.txt (OneDrive / USERPROFILE / TEMP
// fallbacks), flushed after every line. Temporary bring-up scaffolding.

#pragma once

namespace dt::modules::esp::dbg {

// Unconditional line (printf-style). Flushes immediately.
void log(const char* fmt, ...);

// Rate-limited line for hot loops: the same call site prints at most once per
// `min_ms`. Use a unique integer `slot` per call site (0..31).
void log_rl(int slot, unsigned min_ms, const char* fmt, ...);

} // namespace dt::modules::esp::dbg
