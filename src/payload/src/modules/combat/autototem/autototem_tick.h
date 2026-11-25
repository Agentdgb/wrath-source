#pragma once

namespace dt::modules::autototem_tick {

// Drives one 20 Hz step of the AutoTotem state machine. Call every frame with
// the frame delta; it self-throttles.
void pump(float delta_ms);

// Drops all in-flight state and hands back anything the module borrowed.
void reset();

} // namespace dt::modules::autototem_tick
