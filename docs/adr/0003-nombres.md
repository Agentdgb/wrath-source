# ADR 0003 — Centralizar nombres de objetos

## Estado
Aceptado.

## Contexto
Los prefijos de objetos con nombre (mutex centinela, sección de heartbeat) y la
identidad del heartbeat (`magic`, `version`) estaban dispersos en literales en
`StealthDefs.h` y `launcher_injector.h`.

## Decisión
`src/shared/stealth/Naming.h` es la única fuente:
- `kSentinelPrefix` = `Local\wrath_sentinel_`
- `kHeartbeatPrefix` = `Local\wrath_hb_`
- `kHeartbeatMagic` = `0x57525448` ('WRTH')
- `kHeartbeatVersion` = `2`

`StealthDefs.h` incluye `Naming.h` y construye los nombres por PID desde las
constantes. No quedan literales duplicados fuera de `Naming.h`.

## Consecuencias
- Cambiar un prefijo requiere una sola edición.
- El probe conoce los mismos prefijos para detectar los objetos.
