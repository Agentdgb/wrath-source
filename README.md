# Wrath — fuente unificada

Cliente de Minecraft Java 1.21.0–1.21.11 (Fabric) con loader e inyector.

## Árbol

```
src/
  shared/    nt/ stealth/ util/ strings/ diagnostics/   -- utilidades compartidas
  inject/    NtInjector.h  Injector.{h,cpp}  map/  run/  -- API de inyeccion
  loader/    main.cpp  Paths.{h,cpp}  auth/  assets/     -- WrathLoader (WerFault.exe)
  payload/   src/  vendor/  cmake/  test/               -- display_service64.dll
  trace/     bam/pca/prefetch/snapshot/stamps/teardown  -- wrath_trace
tools/
  gate/      wrath_gate.exe
  probe/     wrath_probe.exe
  payload/   scripts de soporte
docs/        arquitectura, ADR, forense, historial
tests/       inject_api_test
```

## Estado verificado del bypass

El loader inyecta por **manual mapping** en memoria (`SEC_COMMIT` pagefile-backed),
resuelve la IAT **dentro del proceso objetivo** y ejecuta desde una región privada.
Esto implica, con honestidad:

- Existe memoria privada ejecutable con una imagen PE — detectable por el
  *loader invariant* (VAD `MEM_PRIVATE` + PE). No se afirma lo contrario.
- No se usa `MEM_IMAGE` propio ni `HIDE_FROM_DEBUGGER` (`kThreadCreateFlags...=0`).
- Las cabeceras PE se pueden limpiar al final (`--no-wipe-headers` lo desactiva).
- No hay "cero páginas privadas ejecutables": el manual mapping las crea.

`tools/probe` mide la huella real. `wrath_probe <pid> --expect clean` sale 0 cuando
no hay hallazgos y 1 cuando los hay.

## Build

Ver `docs/BUILD.md`. Resumen: `cmake --preset x64-release` y
`cmake --build --preset release`.

## Estrategias de inyección

`--strategy=map-private|map-section-image|map-stomp|run-thread|run-hijack|auto`.
Ver `docs/adr/0002-estrategias.md`.
