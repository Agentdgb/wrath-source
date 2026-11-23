# Bypass Obsoleto / Blatant: Borrado de Pánico y Corrupción de $LogFile

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Errores críticos y prácticas destructivas que delatan culpabilidad inmediata ante análisis con NTFS Log Tracker o FTK Imager.

---

## 1. Borrado de Pánico Pre-Screenshare (Shift+Supr / Scripts "Cleaner")
- **Técnica errónea**: En el momento en que un staff o revisor solicita screenshare (en AnyDesk o Discord), el usuario presiona Shift+Supr sobre el loader o ejecuta un archivo `clean.bat` que borra los archivos del escritorio, AppData o Descargas.
- **Por qué falla ante NTFS Log Tracker**:
  - `$LogFile` registra cada transacción con marca de tiempo precisa al milisegundo.
  - Al abrir `$LogFile` en NTFS Log Tracker o Timeline Explorer, el analista filtra por `TransactionType = Delete` y encuentra una ráfaga de archivos eliminados 2 minutos antes de aceptar la llamada de screenshare.
  - Esto constituye prueba forense irrefutable de encubrimiento y destrucción de evidencia (*Evidence Tampering* = Baneo permanente instantáneo).

---

## 2. Intentar Vaciar `$UsnJrnl` Creyendo que Limpia `$LogFile`
- **Técnica errónea**: Ejecutar comandos como:
  ```cmd
  fsutil usn deletejournal /d C:
  ```
- **Por qué falla**:
  - `$UsnJrnl` y `$LogFile` son artefactos NTFS totalmente distintos:
    - `$UsnJrnl` es un registro de cambios de alto nivel para aplicaciones.
    - `$LogFile` es el log de transacciones de bajo nivel del kernel para la integridad del driver NTFS.
  - Ejecutar `deletejournal` elimina el diario USN (lo que en sí mismo ya es una bandera roja gigante de manipulación), pero **deja `$LogFile` intacto al 100%**. El revisor abre `$LogFile` y ve todos los archivos intactos.

---

## 3. Sobrescritura Raw de Sectores de `$LogFile` o Forzar Desmontaje
- **Técnica errónea**: Abrir el handle al volumen físico `\\.\PhysicalDrive0` o `\\.\C:` con privilegios de Administrador para sobrescribir los sectores de `$LogFile` con ceros.
- **Por qué falla**:
  - Provoca corrupción inmediata de la estructura NTFS activa.
  - Windows colapsa con pantalla azul (`BSOD: NTFS_FILE_SYSTEM (0x00000024)`).
  - Al reiniciar, Windows fuerza `chkdsk`, dejando eventos críticos en `System.evtx` y $MFT marcando sectores corruptos.
  - Cualquier sistema con `$LogFile` dañado o en cero es baneado inmediatamente por screensharers profesionales.
