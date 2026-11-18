// PayloadExports.cpp -- Export stub so the DEF file satisfies the linker.
// The EAT LIBRARY name is set to "javaw" in bypass_payload.def, so
// scanners that read IMAGE_EXPORT_DIRECTORY->Name see "javaw" not "bypass_payload".

#include <windows.h>

// Called by the loader (optional) or by any generic injector that knows the export.
// Also serves as the EAT's one named symbol -- keeps the export table non-empty
// so the DEF's LIBRARY directive is emitted.
extern "C" __declspec(dllexport) void PayloadInit() {
    // No-op: real init happens via DllMain -> PayloadThread -> PayloadMain.
    // Callers who know the export can invoke it directly for testing.
}
