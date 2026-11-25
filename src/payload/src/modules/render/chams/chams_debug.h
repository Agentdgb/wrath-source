// chams_debug.h — this module's channel on the shared desktop debug logger.
//
// Writes to <Desktop>\gl_entity_trace.txt. The Desktop is resolved through
// SHGetKnownFolderPath, so it follows a OneDrive redirection instead of
// silently landing in %TEMP% (see util/DeskLog.cpp for why that matters).
//
// Everything routed through here is bring-up scaffolding aimed at answering
// questions that CANNOT be settled by reading code — which JNI names resolve on
// the running MC version, whether the live model pose is reachable, what the
// captured matrices actually contain, and how many players survive each filter
// stage. Those are the questions that otherwise cost an evening of guessing.

#pragma once

#include "util/DeskLog.h"

namespace dt::modules::chams::dbg {

// The channel. Opened on first use; safe to call before anything is installed.
::dt::desklog::Channel ch();

// Slot allocation for the rate-limited and once-only lines. Keeping them in one
// enum is what stops two call sites silently sharing a slot and suppressing
// each other — the bug that makes a log lie by omission.
enum Slot : int {
    kSlotFrame = 0,       // per-frame summary
    kSlotNoCamera,        // camera capture not ready
    kSlotNoWorld,         // world/player null
    kSlotPlayerCount,     // how many players survived each filter
    kSlotPose,            // one player's pose values
    kSlotProject,         // projection results for one player
    kSlotFaces,           // face counts / winding calibration
    kSlotDraw,            // what actually reached the draw list
    kSlotLiveModel,       // live ModelPart availability
    kSlotFallbackNormal,  // Mode::Normal selected -> falling back to Colour
    kSlotGlState,         // GL state snapshot
    kSlotSelfSkip,        // self filtered out
    kSlotRangeSkip,       // out of range
    kSlotCapSkip,         // over max_targets

    // Added after a real failure. The camera-health line was given kSlotFrame,
    // which the "frame:" line already owned, and the forensics summary was given
    // kSlotPlayerCount, which its own header already owned. Each pair fought for
    // one slot and the loser never printed — so the two lines that were supposed
    // to answer the open question were silently absent from every log.
    //
    // This is precisely the failure the comment at the top of this enum warns
    // about, committed anyway. Every rate-limited line now gets its own slot,
    // and nothing shares.
    kSlotCameraHealth,
    kSlotForensics,
};

#define CHAMS_LOG(...)          ::dt::desklog::logf(::dt::modules::chams::dbg::ch(), __VA_ARGS__)
// The `slot` argument is now IGNORED and kept only so the ~20 existing call
// sites did not all have to change. Identity comes from a function-local static
// unique to each call site, so two of them can no longer suppress each other.
//
// That collision happened three times during bring-up. Each time the symptom
// was a log line that simply never appeared, which sent the investigation down
// the wrong path for a whole round trip. Taking the numbering away from the
// programmer is the only fix that stays fixed.
#define CHAMS_RL(slot, ms, ...)                                             \
    do {                                                                    \
        (void)sizeof(slot);   /* accepted and ignored — see above */        \
        static const char kChamsRlSite = 0;                                 \
        ::dt::desklog::logf_site(::dt::modules::chams::dbg::ch(),     \
                                    &kChamsRlSite, (ms), __VA_ARGS__);      \
    } while (0)
#define CHAMS_ONCE(slot, ...)   ::dt::desklog::logf_once(::dt::modules::chams::dbg::ch(), (slot), __VA_ARGS__)
#define CHAMS_BUMP(name)        ::dt::desklog::bump(::dt::modules::chams::dbg::ch(), (name))

} // namespace dt::modules::chams::dbg

