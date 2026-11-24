# ADR 0001 — Árbol único src/tools/docs

## Estado
Aceptado.

## Contexto
El repositorio tenía tres árboles solapados (`wrath/`, `Bypasses/`, `Source/`)
con Includes relativos frágiles, CMakeLists duplicados y binarios trackeados.

## Decisión
Un solo árbol lógico:
- `src/shared` — utilidad sin dependencias de dominio.
- `src/inject` — API de inyección y estrategias.
- `src/loader` — ejecutable `WrathLoader` (`WerFault.exe`).
- `src/payload` — `display_service64.dll` y sus dependencias vendor.
- `src/trace` — captura de artefactos.
- `tools/` — `gate`, `probe`, soporte.
- `docs/` — arquitectura, ADR, forense, historial.

Los duplicados continuados por política de "no borrar código en refactor"
se conservan con sufijo `_legacy` y no se compilan.

## Consecuencias
- Un único include path: `${CMAKE_SOURCE_DIR}/src`.
- `Bypasses/` y `wrath/` desaparecen del índice git.
- Los includes se reescribieron a rutas bajo `shared/`, `inject/`, `loader/`.
