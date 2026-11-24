# Detección Forense: Volume Shadow Copies (VSS / Snapshots de Volumen)

## 1. Identificación y Alcance de la Tecnología
- **Componente**: Volume Shadow Copy Service (VSS / `vssadmin`).
- **Naturaleza**: Subsistema del kernel de Windows que captura instantáneas de bloques de almacenamiento a nivel de bloque en volúmenes NTFS (principalmente `C:\`).
- **Ubicación de Almacenamiento**: Carpeta protegida del sistema `C:\System Volume Information`.
- **Herramientas de Análisis**:
  - `vssadmin list shadows` (inspección de instantáneas desde consola).
  - `ShadowExplorer` (navegación por carpetas históricas en GUI).
  - `FTK Imager` / `OSForensics` (montaje de snapshots forenses).
  - Herramientas de Eric Zimmerman con el modificador `--vss` (`MFTECmd --vss`, `RECmd --vss`, `PECmd --vss`).

---

## 2. Relevancia Forense en Screenshares
1. **Recuperación de Archivos Borrados**:
   - Si un usuario descargó y ejecutó un cheat, y en ese lapso Windows creó un punto de restauración automático o snapshot (por ejemplo, antes de una actualización de Windows o programado por el sistema), el archivo ejecutable, sus logs y configuraciones quedan congelados dentro de la instantánea.
   - Aunque el usuario borre el archivo con `Shift + Supr` o herramientas de sanitización en el sistema activo, el archivo permanece recuperable al 100% dentro de la copia de sombra.
2. **Detección de Discrepancias Temporales (Time-Travel / Tampering)**:
   - Los analistas extraen colmenas históricas (`SYSTEM`, `SOFTWARE`, `NTUSER.DAT`) y registros de eventos (`.evtx`) de los snapshots y los comparan contra las versiones actuales.
   - **Prueba de Encubrimiento**: Si en la instantánea de las 14:00 horas existía una clave de registro en `BAM` o un evento en `Security.evtx`, pero en el sistema activo a las 18:00 horas esa entrada ya no existe, se demuestra matemáticamente que el usuario manipuló o borró registros deliberadamente.
3. **Auditoría de Integridad de Snapshots**:
   - Comprobación de si las copias de seguridad de sombra fueron eliminadas o purgadas deliberadamente antes de la revisión.
