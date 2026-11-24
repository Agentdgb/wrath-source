# Detección Forense: MFTECmd y Análisis de $MFT

## 1. Identificación y Alcance de la Herramienta
- **Herramienta**: MFTECmd (Eric Zimmerman Tools)
- **Objetivo**: Parser forense de bajo nivel del archivo `$MFT` (Master File Table) del sistema de archivos NTFS.
- **Entorno de ejecución**: Análisis offline o en vivo mediante extracción previa de `$MFT` (usando FTK Imager, raw volume read `\\.\C:` o snapshots VSS).
- **Consumo**: Exportación a CSV y visualización estructurada en **Timeline Explorer**.

---

## 2. Artefactos y Metadatos Extraídos
MFTECmd analiza los registros MFT de 1024 bytes y extrae:

1. **Estado de Asignación del Registro (`InUse`)**:
   - `InUse = 1`: Archivo actualmente activo en el sistema de archivos.
   - `InUse = 0`: Archivo marcado como **eliminado**. La MFT conserva el registro, nombre, rutas, marcas de tiempo y fragmentos hasta que otro archivo sobrescriba ese slot.
2. **Atributos de Marcas de Tiempo Duales**:
   - `$STANDARD_INFORMATION` ($SI): Modificado por APIs estándar de Windows (`SetFileTime`).
   - `$FILE_NAME` ($FN): Modificado únicamente por el kernel de Windows al renombrar, mover o crear el archivo.
3. **Jerarquía y Parentesco**:
   - `Record Number` y `Parent Record Number`: Permite reconstruir con precisión milimétrica la ruta completa de carpetas donde residió un archivo, incluso si la carpeta fue borrada.
4. **Alternate Data Streams (ADS)**:
   - Identifica streams ocultos, especialmente `:Zone.Identifier` (ZoneId=3 indica descarga desde internet/navegador).
5. **Tamaño y Datos Residentes**:
   - Archivos pequeños (< 700 bytes) se almacenan como *residentes* directamente dentro del registro MFT, preservando su contenido incluso tras ser borrados.

---

## 3. Heurísticas y Reglas de Flag en Screenshares (Echo, Paladin, Ocean, Avenge)
Los inspectores forenses aplican las siguientes reglas automatizadas sobre la salida de MFTECmd:

- **Flag 1 — Timestomping Discrepancy**: Si `$SI.CreationTime < $FN.CreationTime` o `$SI` tiene nanosegundos en cero (`.0000000Z`) mientras `$FN` tiene precisión completa.
- **Flag 2 — Deleted Executables in Userland**: Registros con `InUse = 0` cuyo nombre corresponde a clientes, loaders, ejecutables en `AppData`, `Temp`, `Downloads` o escritorios, eliminados recientemente.
- **Flag 3 — Parent Orphan / Path Discrepancy**: Registro activo o borrado en carpetas de sistema sospechosas o directorios temporales que intentaron enmascarar su extensión (`.dll` renombrado a `.dat`).
- **Flag 4 — USN / MFT Sequence Mismatch**: Inconsistencias entre los números de secuencia del registro MFT y las transacciones registradas en `$UsnJrnl`.
