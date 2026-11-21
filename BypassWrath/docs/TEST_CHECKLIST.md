# Checklist de test

## Reglas de operación (R1 / R2)

**R1** — Al terminar el test, los once servicios deben estar `RUNNING`:
`DPS`, `DiagTrack`, `AppInfo`, `Dusmsvc`, `WSearch`, `BAM`, `SysMain`, `PcaSvc`,
`EventLog`, `Schedule`, `DcomLaunch`. Excepción solo en CustomOS.

**R2** — No desactivar servicios, no bloquear descargas, no usar lolbins
blatants (`msbuild`, `mshta`, `rundll32`, `regsvr32`, `wmic`, `bitsadmin`,
`curl`, `certutil`). Restart de servicio cuenta como evidencia (7036).

La única excepción permitida es el stop/start de DPS dentro de `ScrubSrum`, con
pre-flight y watchdog. Si el revisor marca el 7036: `--no-srum`.

## Verificación

1. **Sin `--scrub` en 4688.** Filtrar eventos 4688 de `Security` en la ventana
   del test. El child debe mostrar solo la ruta del exe, sin argumentos.
2. **Cero lolbins en la línea de tiempo.** Ningún evento 4688 con `msbuild`,
   `mshta`, `rundll32`, `regsvr32`, `wmic`, `bitsadmin`, `curl` o `certutil`.
   Tampoco `cmd.exe` del punto de limpieza.
3. **MEM_PRIVATE confirmado.** En `javaw.exe`, el módulo mapeado debe aparecer
   como `MEM_PRIVATE` (SEC_COMMIT), nunca `MEM_IMAGE` respaldado por archivo.
4. **Strings a cero.** Volcar la imagen mapeada y confirmar que no aparecen
   `bypass_payload`, `PayloadInit`, `JNI_OnLoad`, `BypassWrath`, `bp::` ni `.?A`.
5. **Once servicios `RUNNING` al final.** `powershell tools\verify.ps1`.
6. **DPS sin 7036** (o, si se usó SRUM, una sola pareja stop/start documentada).
7. **Prefetch propio borrado.** Sin `RuntimeBroker-*.pf` tras la salida del child.
8. **`PendingFileRenameOperations` sin entradas** del loader.
9. **Clave `TMP_BWAMC` inexistente** tras `ScrubAmcache`.
10. **Pca values borrados** en Store / Persisted / Layers; `LastWriteTime`
    restaurado.
11. **Ejecución desde `%TEMP%`**, nunca Desktop / Downloads / Documents.

## Ejecución

```
powershell -ExecutionPolicy Bypass -File tools\verify.ps1
```

El script cubre los puntos 1, 2 y 5. Los puntos 3, 4, 7, 8, 9, 10 y 11 son
comprobación manual con Process Hacker / pe-sieve / Registry Editor.
