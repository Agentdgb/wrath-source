# Bypass Obsoleto / Blatant: Borrado Naive de Registro y Vaciado de Claves

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Técnicas destructivas o ingenuas que garantizan una detección del 100% al ser analizadas con RegistryExplorer y rla.

---

## 1. Borrado Directo con `RegDeleteValue` / `reg delete` / PowerShell
- **Técnica errónea**:
  ```cmd
  reg delete "HKLM\SYSTEM\CurrentControlSet\Services\bam\State\UserSettings\S-1-5-..." /v "\Device\HarddiskVolume3\...\loader.exe" /f
  ```
  o llamar directamente a `RegDeleteValueW()` sin sobrescritura previa.
- **Por qué falla ante RegistryExplorer / rla**:
  - `RegDeleteValue` simplemente desvincula la celda `vk` de la lista enlazada del bloque `hbin`.
  - El nombre completo de la ruta (`C:\...\WrathLoader.exe`) y su tipo de dato quedan intactos en el espacio no asignado (slack space).
  - Al ejecutar `rla.exe` para aplicar transacciones y cargar el hive en `RegistryExplorer`, la herramienta resalta automáticamente el valor borrado en la sección **"Associated deleted records"**.
  - Además, la clave padre actualiza su `LastWriteTime` al segundo exacto del borrado, evidenciando manipulación minutos antes de la revisión forense.

---

## 2. Vaciado Completo de Claves (BAM, AppCompatCache, UserAssist)
- **Técnica errónea**: Borrar la subclave completa de BAM del usuario o eliminar todas las entradas de `UserAssist` o `AppCompatCache` para "limpiar todo rastro".
- **Por qué falla (Flag Inmediato)**:
  - Todo sistema operativo Windows funcional acumula cientos de entradas legítimas en BAM (`explorer.exe`, `chrome.exe`, `discord.exe`, etc.) y UserAssist.
  - Una clave BAM completamente vacía o con 1 solo elemento es un indicador flagrante de limpieza de registros (clasificado automáticamente como *Evidence Tampering / Cleared Artifacts* en Echo y Paladin).

---

## 3. Borrado o Truncado de Archivos de Transacción (`.LOG1`, `.LOG2`)
- **Técnica errónea**: Intentar forzar el borrado de los archivos `SYSTEM.LOG1`, `NTUSER.DAT.LOG1` desde entornos live o WinPE.
- **Por qué falla**:
  - Causa inconsistencias graves en el hive que obligan a Windows a marcar el registro como recuperado ("Registry Hive Recovery Event").
  - Deja huecos evidentes en el USN Journal y registros MFT borrados para los archivos de log.
