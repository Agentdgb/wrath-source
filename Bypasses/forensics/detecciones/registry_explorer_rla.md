# Detección Forense: RegistryExplorer y rla (Registry Transaction Logs & Slack Space)

## 1. Identificación y Alcance de las Herramientas
- **Herramientas**: `RegistryExplorer.exe` y `rla.exe` (Registry Log Applier) — Suite Eric Zimmerman.
- **Objetivo**: Extracción, reproducción de logs de transacciones del registro (`.LOG`, `.LOG1`, `.LOG2`) y análisis de celdas desasignadas (unallocated / slack space) en colmenas del registro de Windows (`SYSTEM`, `NTUSER.DAT`, `SOFTWARE`, `UsrClass.dat`).
- **Capacidades Especiales**:
  - Reconstrucción de transacciones sucias no consolidadas en disco (`rla.exe`).
  - Detección de entradas, claves y valores **eliminados** (`Associated deleted records` en RegistryExplorer).
  - Inspección de marcas de tiempo de última escritura por clave (`LastWriteTime`).

---

## 2. Artefactos Clave Auditados

### A. En la colmena `SYSTEM` (`C:\Windows\System32\config\SYSTEM`):
1. **BAM (Background Activity Moderator)**:
   - Ruta: `SYSTEM\CurrentControlSet\Services\bam\State\UserSettings\<SID>`
   - Contiene la ruta completa de cada ejecutable iniciado y su timestamp de ejecución en formato FILETIME (64-bit).
2. **AppCompatCache (Shimcache)**:
   - Ruta: `SYSTEM\CurrentControlSet\Control\Session Manager\AppCompatCache`
   - Historial de ejecutables presentes en el sistema para compatibilidad de aplicaciones.
3. **MountedDevices**:
   - Mapeo de volúmenes, GUIDs y letras de unidad asignadas en el sistema.

### B. En la colmena `NTUSER.DAT` (`C:\Users\%username%\NTUSER.DAT`):
1. **ComDlg32 (Diálogos Comunes de Archivos)**:
   - `Software\Microsoft\Windows\CurrentVersion\Explorer\ComDlg32\OpenSavePidlMRU`: Historial de archivos seleccionados mediante cuadros de diálogo Abrir/Guardar.
   - `Software\Microsoft\Windows\CurrentVersion\Explorer\ComDlg32\LastVisitedPidlMRU`: Últimas carpetas exploradas por cada ejecutable.
2. **UserAssist**:
   - `Software\Microsoft\Windows\CurrentVersion\Explorer\UserAssist\{GUID}\Count`: Ejecución de aplicaciones GUI mediante el Explorador de Windows (nombres ofuscados en ROT13, recuento de ejecuciones y último uso).
3. **Compatibility Assistant (Store)**:
   - `Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Compatibility Assistant\Store`: Rutas de ejecutables evaluados por PCA.

---

## 3. Mecanismo de Detección de Registros Eliminados (Slack Space / Unallocated Cells)
- Windows almacena el registro en bloques denominados `hbins`. Cada clave (`nk`) o valor (`vk`) ocupa una celda de tamaño específico.
- Cuando un usuario o programa llama a `RegDeleteValue` o `RegDeleteKey`:
  1. Windows **NO sobrescribe** los bytes de la celda. Solo cambia la longitud de la celda a negativo (marca de celda libre) y la desvincula de la lista activa.
  2. La transacción se anota en el log de transacciones (`.LOG1` / `.LOG2`).
- **Qué hace RegistryExplorer**:
  - Escanea el espacio no asignado (slack space) de los bloques `hbins`.
  - Extrae los nombres, tipos de datos y valores que quedaron huérfanos.
  - Al abrir una clave como `BAM` o `OpenSavePidlMRU`, en la pestaña inferior **"Associated deleted records"**, RegistryExplorer muestra en color rojo o diferenciado los nombres de ejecutables eliminados recientemente (`WrathLoader.exe`, `cheat.exe`, etc.) junto con su timestamp original.
- **Detección por `LastWriteTime`**:
  - Si un valor de BAM fue borrado a las 18:30:15 y el juego inició a las 18:30:10, la clave padre `UserSettings\<SID>` reflejará `LastWriteTime = 18:30:15`, probando manipulación segundos antes de la revisión.
