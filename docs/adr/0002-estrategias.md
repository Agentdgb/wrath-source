# ADR 0002 — Estrategias de inyección seleccionables

## Estado
Aceptado.

## Contexto
Existían tres implementaciones de mapeo (private, TxF-section, stomp) y dos
variantes de ejecución (thread, hijack) repartidas entre `NtInjector.cpp` y
`ManualMap.h`, sin una interfaz común.

## Decisión
API única en `src/inject/Injector.h`:
- `Strategy`: `Auto`, `MapPrivate`, `MapSectionImage`, `MapStomp`, `RunThread`, `RunHijack`.
- `Options`: `unlink`, `wipe_headers`, `reprotect`, `zero_scratch`, `hide_thread`.
- `Attempt` / `Result`: resultado por intento con detalle y duración.
- `inject()` recorre la tabla en modo `Auto` hasta que una estrategia tiene éxito.

Cada estrategia vive en su propio `.cpp` con la firma
`bool(HANDLE, std::span<const uint8_t>, const Options&, Result&)`.

## Flags
`--strategy=<nombre>`, `--no-unlink`, `--no-wipe-headers`, `--no-reprotect`,
`--no-zero-scratch`, `--no-hide-thread`.

## Consecuencias
- `stomp` puede fallar por tamaño; devuelve detalle en `Result::detail`, nunca lanza.
- `NtInjector.cpp` queda como unidad vacía (histórica).
- Verificación: `tests/inject_api.cpp`.
