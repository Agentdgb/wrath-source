# BypassWrath

Loader / inyector para Minecraft Java Edition. Carga el payload desde un recurso
RCDATA cifrado, lo mapea en memoria con `SEC_COMMIT` (sin archivo en disco) y
encadena la limpieza forense en un proceso hijo sin argumentos de línea de
comandos.

## Reglas de operación (R1 / R2)

**R1 — Servicios que deben quedar `RUNNING` al terminar el test:**

`DPS`, `DiagTrack`, `AppInfo`, `Dusmsvc`, `WSearch`, `BAM`, `SysMain`, `PcaSvc`,
`EventLog`, `Schedule`, `DcomLaunch`.

Excepción únicamente en CustomOS. Si cualquiera de estos once servicios no está
`RUNNING` al final, el test falla.

**R2 — Prohibido:**

- Desactivar servicios.
- Bloquear descargas.
- Usar lolbins blatants: `msbuild`, `mshta`, `rundll32`, `regsvr32`, `wmic`,
  `bitsadmin`, `curl`, `certutil`.

Un restart de servicio cuenta como evidencia: deja el evento 7036. La única
excepción permitida es el stop/start de DPS dentro de `ScrubSrum`, blindado con
pre-flight y watchdog. Si un revisor marca ese 7036, usar `--no-srum` y
documentar SRUDB como gap aceptado.

Estas dos reglas también están al inicio de `docs/TEST_CHECKLIST.md`.

## Build

```
build.bat
cmake --build build --config Release --parallel
```

Salida en `bin/Release/`:

- `RuntimeBroker.exe` — loader silencioso, modo automático (un clic, sin flags).
- `BypassLoaderDbg.exe` — variante con consola, para pruebas.
- `bypass_payload.dll` — payload manual-mapeado.
- `test_payload.dll` — DLL de verificación del CRT / TLS / SEH / heap.

## Uso

Abrir Minecraft Java y ejecutar `RuntimeBroker.exe`. El loader resuelve
`javaw.exe`, inyecta, lanza el hijo de limpieza, cera los bytes del payload y se
auto-elimina. Flags opcionales: `--pid <n>`, `--log`, `--no-srum`, `--srum-all`,
`--srum-only`. El hijo de limpieza no recibe ningún argumento: el contexto va en
la sección con nombre `Local\BypassWrathScrubCtx`.

## Estructura

| Ruta | Contenido |
|---|---|
| `loader/src/main.cpp` | entry points, targeting, pipeline automático |
| `loader/src/AutoPipeline.h` | registry de comandos named + executor |
| `loader/src/ManualMap.h` | `ManualMapMemoryOnly`, `ScrubStrings`, PEB→Ldr |
| `loader/src/ForensicScrub.h` | BAM, MuiCache, UserAssist, Pca, Amcache, WER, SRUM |
| `loader/src/SelfDelete.h` | borrado ADS + POSIX |
| `payload/src/` | payload, TLS thread, .pdata dinámico, puente JNI |
| `tools/` | embed cifrado, scrub RTTI, verificación |

## Operación

Ver `docs/OPERATIONS.md`. Ejecutar siempre desde `%TEMP%`.
