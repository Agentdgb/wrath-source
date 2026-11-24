# Bypass Limpio: Evasión de Comandos Interactivos (CMD / PowerShell en Screenshares)

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que el sistema supere al 100% las inspecciones interactivas ejecutadas en vivo por los revisores, manteniendo una coherencia total con las respuestas esperadas de un sistema legítimo.

---

## 1. Vector Principal: Estado Impecable de Servicios (`services.cpp`)
*Ubicación en el código:* `Bypasses/gate/src/services.cpp` (`kMandatoryServices`).

1. **Garantía de Estado Running**:
   - Wrath cuenta con una rutina de verificación interna (`audit_services()`) que asegura que los 11 servicios obligatorios estén activos y saludables:
     `DcomLaunch`, `RpcSs`, `EventLog`, `Schedule`, `AppInfo`, `DPS`, `DiagTrack`, `DUSMSvc`, `WSearch`, `SysMain`, `PcaSvc`.
2. **Respuesta en Screenshare**:
   - Al ejecutar el comando habitual del revisor:
     ```powershell
     get-service | findstr -i "pcasvc"; get-service | findstr -i "DPS"; get-service | findstr -i "Diagtrack"; get-service | findstr -i "sysmain"; get-service | findstr -i "eventlog"
     ```
   - Cada uno de los servicios devuelve sin excepción el estado **`Running`**.
   - Cero discrepancias, cero sospechas.

---

## 2. Evasión de `forfiles /P C:\ /S /D -1` (Aislamiento VHD y Timestomping)
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1` y `Bypasses/trace/stamps.cpp`.

1. **Aislamiento de Volumen**:
   - `forfiles /P C:\` opera restringido exclusivamente al árbol de directorios del volumen `C:\`.
   - WrathLoader y WrathPayload residen dentro del contenedor VHD montado sin letra de unidad (`NON_PNP`). Los archivos internos del VHD **no pertenecen al volumen `C:\`**, por lo que `forfiles` no los enumera.
2. **Marcas Temporales Clonadas (`restore_stamps`)**:
   - Cualquier archivo señuelo temporal que deba tocar el disco se sincroniza en `$STANDARD_INFORMATION` y `$FILE_NAME` con marcas de fecha de hace meses o años (clonadas de `robocopy.exe`).
   - El parámetro `/D -1` (archivos modificados en las últimas 24 horas) **ignora por completo** estos binarios, omitiéndolos de la salida de la consola.

---

## 3. Invisibilidad en `Win32_Process` y `tasklist /svc`
*Diseño del Loader Wrath:*

1. **Proceso Efímero de Inyección**:
   - `WrathLoader.exe` se inicia, realiza la inyección manual en memoria del proceso de Minecraft (`javaw.exe`) y se autodestruye/cierra inmediatamente (`ExitProcess`).
   - Durante la sesión de juego y el screenshare, **el loader ya no existe en la lista de procesos**.
2. **Proceso Objetivo Legítimo**:
   - Al ejecutar `Get-WmiObject Win32_Process | Select Name, CommandLine`, la única aplicación relevante visible es `javaw.exe` ejecutando el cliente normal de Fabric de Minecraft con sus argumentos legítimos de JVM.
   - Al estar el payload inyectado mediante manual mapping sin nuevo proceso, no existe ningún proceso anómalo que consuma CPU ni que figure en `tasklist`.

---

## 4. Integridad de Sistema de Archivos (`fsutil dirty query`)
1. Wrath no corrompe sectores ni fuerza cierres abruptos de NTFS.
2. Al ejecutar:
   ```cmd
   fsutil dirty query C:
   ```
   La respuesta es limpia: **`Volume - C: is NOT Dirty`**.
