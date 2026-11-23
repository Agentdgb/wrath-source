# Generated mapping tables

**Post-pivot: Fabric-only, all embedded in the DLL — no runtime disk I/O.**

Every supported MC point release ships as one translation unit here
(`mappings_1_21_N.cpp`) with four sorted `constexpr` arrays and one exported
`constinit const gen::VersionTable v_1_21_N`. `mappings_index.cpp` chains the
per-version exports into a version-string → table lookup.

Runtime lookup (`src/mappings/MappingRegistry.cpp`): FNV1a-64 the canonical
string, `std::lower_bound` over the sorted array. All strings live in `.rdata`
inside `vantaclient.dll`.

## Files

| File | Owner | Purpose |
| --- | --- | --- |
| `GenTable.h`                | hand-written  | POD types shared by every generated TU + the runtime |
| `VersionIndex.h`            | hand-written  | `findVersionTable`/`defaultVersionTable`/`allVersionTables` declarations |
| `mappings_index.cpp`        | **generated** | `extern` decls + `IndexRow[]` + the lookup |
| `mappings_1_21_N.cpp` (×12) | **generated** | Per-version tables + `constinit const VersionTable v_1_21_N` |

Until you run the generator once, `mappings_index.cpp` is a **placeholder**
that defines empty tables for every version so the DLL links but resolves
nothing. Modules degrade to "unresolved" traces; the client still boots.

## Regenerate

```powershell
cmake -S . -B build -DVANTA_REGEN_MAPS=ON
cmake --build build --target vanta_genmaps
```

Then rebuild `vantaclient` — CMake's `CONFIGURE_DEPENDS` glob picks up the
newly emitted files without you having to touch `CMakeLists.txt`.

See `tools/genmaps/README.md` for the full pipeline and how to add new MC
versions or new canonical names.
