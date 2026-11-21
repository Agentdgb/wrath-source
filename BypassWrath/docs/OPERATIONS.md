# Operación

## Reglas R1 / R2

Ver `README.md`. Resumen: once servicios en `RUNNING` al final; cero lolbins,
cero desactivación de servicios, cero bloqueo de descargas.

## Dónde ejecutar

**Siempre desde `%TEMP%`.** Nunca desde `Desktop`, `Downloads` ni `Documents`.

Razón: WSearch indexa esas ubicaciones por defecto y `Windows.edb` no se puede
editar en caliente sin parar el servicio (prohibido por R1). `%TEMP%` no está
indexado por defecto, así que el ejecutable nunca entra en el índice.

Lo mismo aplica a SysMain/ReadyBoot: su trace DB no se puede editar en caliente.
Ambos se documentan como **gap aceptado**, mitigado solo por la ubicación.

## SRUM

`SRUDB.dat` se sanea con ESE dentro de `ScrubSrum`. DPS se para y se reinicia;
eso genera el evento 7036 y cuenta como evidencia según R2.

- Pre-flight: si DPS no está `RUNNING`, no se toca y el paso se reporta fallido.
- Watchdog armado antes del STOP: fuerza `StartService` si algo falla a mitad.
- Si un test concreto marca el 7036: ejecutar con `--no-srum`. SRUDB queda como
  gap documentado y DPS no se toca en absoluto.

## Lolbins

No usar `msbuild`, `mshta`, `rundll32`, `regsvr32`, `wmic`, `bitsadmin`, `curl`
ni `certutil`. El child de limpieza se lanza con `CreateProcessW` sobre el propio
binario y `lpCommandLine = nullptr`: su 4688 muestra solo la ruta del exe, sin
`--scrub`. Borra su propio `.pf` directamente, sin `cmd.exe` intermedio.

## PcaSvc

Se mantiene `RUNNING`. `ScrubPca()` solo edita tres claves de registro bajo
`HKCU\...\AppCompatFlags` y restaura `LastWriteTime`.

## PendingFileRenameOperations

`SelfDelete()` usa ADS rename + POSIX delete. No hay fallback a
`MoveFileExW(MOVEFILE_DELAY_UNTIL_REBOOT)`: esa ruta escribiría
`PendingFileRenameOperations` en HKLM y sobreviviría al reinicio.

## Gaps documentados

- WSearch / `Windows.edb` — mitigado por ubicación (`%TEMP%`).
- SysMain / ReadyBoot trace DB — mitigado por ubicación.
- SRUDB — mitigado por `--no-srum` si el 7036 es observable.
