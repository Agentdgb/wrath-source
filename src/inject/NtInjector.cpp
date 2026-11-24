// NtInjector.cpp -- legacy aggregation unit.
//
// All injection bodies were migrated to:
//   map/MapPrivate.cpp, map/MapSectionImage.cpp, map/MapStomp.cpp,
//   run/RunThread.cpp, run/RunHijack.cpp, and Injector.cpp.
// The public API now lives in inject/Injector.h.
// This translation unit is intentionally empty; it remains listed in the
// loader CMake sources to preserve the historical target layout.

#include "inject/Injector.h"
