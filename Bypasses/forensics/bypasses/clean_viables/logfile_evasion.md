# Bypass Limpio: Evasión de $LogFile

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Dado que `$LogFile` no puede ser borrado mediante APIs de Windows sin corromper el sistema de archivos, el bypass limpio se basa en la **prevención absoluta de escrituras en el volumen físico host** y el uso de capas de almacenamiento desacopladas.

---

## 1. Vector Principal: Aislamiento en Contenedor VHD Volátil (M08)
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1` y `teardown_vhd_environment.ps1`.

### Mecanismo:
1. En lugar de descomprimir, compilar o ejecutar Wrath en el volumen NTFS principal (`C:\`), el entorno se ejecuta dentro de un disco virtual VHD (`Backup_2026.vhd`).
2. **Aislamiento de Transacciones NTFS**:
   - Cada sistema de archivos NTFS tiene su **propio `$LogFile` independiente** a nivel de volumen.
   - Las operaciones de creación de `WrathLoader.exe`, escritura de perfiles, mapeo de `WrathPayload.dll` y su posterior cierre ocurren dentro del sistema de archivos interno del archivo VHD.
   - En el `$LogFile` de `C:\` (el volumen auditado por FTK Imager y NTFS Log Tracker durante el screenshare), **no existe una sola transacción de los archivos del cliente**. Solo figura la lectura o montaje del contenedor VHD como un archivo monolítico legítimo.
3. Al desmontar el VHD con `AttachVirtualDisk` teardown, el volumen desaparece y no hay transacciones de borrado de ejecutables registradas en `C:\$LogFile`.

---

## 2. Inyección Directa en Memoria / Payload Streamed
*Mecanismo:* Inyector nativo en memoria (`NtInjector.cpp` / `ManualMap.h`).

1. Si el loader se entrega a través de la red o se compila en un entorno aislado, el payload (`WrathPayload.dll`) se transfiere directamente al espacio de memoria de `javaw.exe` (Minecraft Fabric) mediante asignaciones en memoria (`NtAllocateVirtualMemory` + `NtWriteVirtualMemory`).
2. Al no crearse nunca el archivo `.dll` en disco, el driver `ntfs.sys` **no genera transacciones en `$LogFile`**.

---

## 3. Roll-Over Circular Pasivo (Sobrescritura Natural)
1. Para usuarios que ejecutaron binarios en el pasado y requieren que el rastro desaparezca de `$LogFile` sin levantar banderas:
2. **Buffer Circular**:
   - El tamaño de `$LogFile` es finito (habitualmente 64 MB).
   - Generar tráfico normal y legítimo de disco mediante operaciones comunes del sistema operativo (actualizaciones de Windows, descargas de Steam/Epic, uso regular de navegadores) satura el buffer de transacciones.
   - Las transacciones antiguas se van sobreescribiendo de manera totalmente orgánica y legal según el diseño nativo de NTFS, sin dejar eventos de manipulación forense ni banderas rojas.
