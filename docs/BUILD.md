# Build

## Requisitos
- Visual Studio 2022 o superior (el entorno actual usa VS 18 2026; el preset
  `generator` se ajustó en consecuencia — el work order pedía VS 17 2022).
- Windows SDK 10.0.26100+.
- JDK 17+ con `include/jni.h` (para los headers JNI del payload).

## Presets

```
cmake --preset x64-debug
cmake --preset x64-release
cmake --preset x64-relwithdebinfo
```

Generan en `build/<presetName>`. Binarios en `build/<preset>/dist`.

## Compilar

```
cmake --build --preset release
cmake --build --preset debug
cmake --build --preset relwithdebinfo
```

Targets:
- `WrathLoader` → `WerFault.exe`
- `WrathPayload` (alias de `display_service`) → `display_service64.dll`
- `wrath_gate` → `wrath_gate.exe`
- `wrath_probe` → `wrath_probe.exe`
- `wrath_trace`, `wrath_inject` → librerías estáticas

## Tests

```
cmake -S . -B build/x64-tests -G "Visual Studio 18 2026" -A x64 -DWRATH_BUILD_TESTS=ON
cmake --build build/x64-tests --config Release --target inject_api_test
ctest --test-dir build/x64-tests -C Release
```

`inject_api_test` verifica la API de inyección sin tocar un proceso real:
estrategias, `inject()` con PE inválido y con handle nulo.

## Probe

```
wrath_probe <pid> --expect clean
wrath_probe <pid> --json
```

Sale 0 con `--expect clean` si no hay hallazgos, 1 si los hay. Las comprobaciones
son de solo lectura: VAD, PE en privada, PEB vs VAD, hilos, objetos con nombre
y strings. Ver `docs/DETECTION-MAP.md`.

## Estrategias de inyección

```
WerFault.exe --strategy=map-private
WerFault.exe --strategy=map-stomp --no-wipe-headers
WerFault.exe --strategy=auto
```

Flags: `--no-unlink`, `--no-wipe-headers`, `--no-reprotect`,
`--no-zero-scratch`, `--no-hide-thread`.
