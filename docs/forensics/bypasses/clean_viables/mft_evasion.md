# Bypass Limpio: Evasión de MFTECmd y Aislamiento $MFT

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass garantiza que ningún artefacto forense del loader ni del payload quede registrado en el archivo `$MFT` del disco host (`C:\$MFT`), evitando detecciones por registros borrados (`InUse = 0`) o inconsistencias de marcas de tiempo.

---

## 1. Vector Principal: Aislamiento por Contenedor VHD Volátil (M08)
*Implementado en:* `Bypasses/tools/setup_vhd_environment.ps1` y `teardown_vhd_environment.ps1`.

### Mecanismo:
1. Se crea un disco virtual dinámico (`.vhd`) con un nombre de señuelo legítimo de Windows (e.g., `C:\System_Backups\Backup_2026.vhd`).
2. Se monta mediante llamadas nativas a la API Win32 de `virtdisk.dll` (`OpenVirtualDisk` y `AttachVirtualDisk`) utilizando los flags:
   - `ATTACH_VIRTUAL_DISK_FLAG_NO_LOCAL_HOST (0x08)`
   - `ATTACH_VIRTUAL_DISK_FLAG_NON_PNP (0x40)`
   - `ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER (0x02)`
3. La partición interna del VHD se enlaza a un **Volume Mount Point** en una carpeta legítima del sistema (e.g. `%LOCALAPPDATA%\Microsoft\Windows\SystemDataCache`), sin letra de unidad.
4. **Resultado en $MFT del Host**:
   - El volumen host `C:\` solo registra la creación de un único archivo contenedor (`Backup_2026.vhd`).
   - El ejecutable del loader (`WrathLoader.exe` / `BackupManager.exe`) y el payload (`WrathPayload.dll` / `display_service64.dll`) residen y se ejecutan **exclusivamente dentro de la MFT propia del VHD**.
   - Al ejecutar `MFTECmd -f C:\$MFT`, **no existe ningún registro** del loader ni del payload en la tabla de archivos de `C:\`.
5. **Teardown**: Al desmontar y purgar el contenedor VHD mediante desasignación limpia, la MFT del host permanece inmaculada.

---

## 2. Vector Secundario: Inyección Reflectiva / Manual Mapping en Memoria
*Implementado en:* `Bypasses/injector/ManualMap.h` y `wrath/loader/`.

### Mecanismo:
1. El payload (`WrathPayload.dll`) no se escribe en el disco NTFS como archivo individual desprotegido.
2. Si el loader se ejecuta desde memoria o canal seguro (pipe en memoria / payload incrustado en recursos cifrados `.rdata`), se inyecta directamente en el espacio de memoria de `javaw.exe` (Minecraft Fabric) mediante manual mapping.
3. No se produce llamada a `CreateFileW` ni `WriteFile` para el DLL en el disco de la víctima.
4. MFTECmd tiene visibilidad nula sobre memoria RAM; al no existir archivo DLL en disco, no hay registro MFT que analizar.

---

## 3. Sincronización Dual $STANDARD_INFORMATION y $FILE_NAME
*Implementado en:* `Bypasses/trace/stamps.cpp` (`restore_stamps`).

Si temporalmente se requiere colocar un binario en disco antes de la inyección:
1. Las herramientas forenses detectan el timestomping tradicional porque `SetFileTime` solo altera `$STANDARD_INFORMATION`.
2. Para lograr un bypass indetectable ante MFTECmd:
   - Se modifica `$STANDARD_INFORMATION` vía `SetFileTime`.
   - Se sincroniza `$FILE_NAME` utilizando `NtSetInformationFile` con la clase `FileBasicInformation` a nivel de NTAPI.
   - Se clonan con precisión de 100 nanosegundos las marcas de un binario legítimo del sistema en el mismo directorio (e.g., `robocopy.exe`), eliminando cualquier nanosegundo en `.0000000Z` o discrepancia `$SI < $FN`.
