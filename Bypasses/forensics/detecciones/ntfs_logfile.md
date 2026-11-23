# Detección Forense: NTFS $LogFile y Transacciones del Sistema de Archivos

## 1. Identificación y Alcance del Artefacto
- **Artefacto**: `$LogFile` (NTFS Metadata File 2).
- **Objetivo**: Registro circular de transacciones a nivel de sector del sistema de archivos NTFS implementado por el driver `ntfs.sys`.
- **Diferencia crítica con $UsnJrnl**:
  - `$UsnJrnl` posee APIs públicas en Windows (`FSCTL_DELETE_USN_JOURNAL`) para ser vaciado.
  - `$LogFile` **NO tiene interfaz pública de borrado**. El kernel lo mantiene permanentemente activo y bloqueado para garantizar la consistencia ACID (Atomicidad, Consistencia, Aislamiento, Durabilidad) ante cortes de energía o caídas del sistema.
- **Herramientas de Análisis**:
  - `NTFS Log Tracker`
  - `FTK Imager` (extracción a bajo nivel de `$LogFile`)
  - `Timeline Explorer` (visualización y filtrado de eventos)

---

## 2. Métricas y Operaciones Registradas en $LogFile
`$LogFile` registra las operaciones físicas y lógicas del sistema de archivos divididas en operaciones **Redo** y **Undo**:

1. **Creación, Modificación y Renombrado**:
   - Registro de transacciones con nombre del archivo, ruta y cambio de atributos en los bloques MFT correspondientes.
2. **Eliminación Atómica**:
   - Cuando un archivo es eliminado, `$LogFile` registra la transacción completa de desasignación de clusters (`UpdateFileName`, `DeleteFile`, `FreeClusters`), conservando el nombre del archivo y el instante exacto de la operación.
3. **Discriminación de Contexto (Usuario vs Sistema)**:
   - Permite a los analistas forenses diferenciar entre transacciones automatizadas en segundo plano (e.g., telemetría del sistema, actualizaciones de Windows) y acciones manuales de usuario (navegar por carpetas, ejecutar, renombrar o borrar archivos).
4. **Buffer Circular**:
   - Tiene un tamaño predeterminado fijo (típicamente entre 2 MB y 64 MB). Cuando se llena, las nuevas transacciones sobrescriben las más antiguas de forma circular.

---

## 3. Heurísticas y Reglas de Flag en Screenshares
Los analistas forenses (Echo, Paladin, Ocean, o revisiones manuales con NTFS Log Tracker) buscan:

- **Flag 1 — Actividad de Borrado Inmediata Pre-Screenshare**: Registros de transacciones de borrado (`DeleteFileRecord`) ocurridas en los 10 a 30 minutos previos a la llamada de screenshare o mientras el revisor espera que el usuario acepte AnyDesk/Discord.
- **Flag 2 — Renombrado o Movimiento Rápido de Ejecutables**: Transacciones que evidencian el cambio de nombre de un `.exe` o `.dll` hacia extensiones arbitrarias (`.tmp`, `.dat`, `.png`) o su desplazamiento hacia carpetas remotas.
- **Flag 3 — Discrepancia Pantalla vs Disco**: Archivos manipulados mientras el usuario tenía la pantalla congelada o ventanas minimizadas.
- **Flag 4 — Transacciones de "Disk Cleaners"**: Secuencias masivas de eliminación y actualización de registros MFT en intervalos de milisegundos, típicas de scripts "cleaner.bat" o software de limpieza destructivo.
