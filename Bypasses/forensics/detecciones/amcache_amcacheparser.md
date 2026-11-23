# Detección Forense: AmCache (Amcache.hve) y AmcacheParser

## 1. Identificación y Alcance del Artefacto
- **Artefacto**: AmCache (`Amcache.hve` y logs asociados `Amcache.hve.LOG1`, `Amcache.hve.LOG2`).
- **Ubicación en Disco**:
  `%SystemRoot%\AppCompat\Programs\Amcache.hve`
- **Naturaleza**: Colmena del registro independiente que reemplazó a `RecentFileCache.bcf`. Es gestionada por la infraestructura de compatibilidad de aplicaciones de Windows (`Application Experience`) y tareas de telemetría (e.g. `Microsoft\Windows\Application Experience\ProgramDataUpdater`).
- **Herramientas de Análisis**:
  - `AmcacheParser.exe` (Eric Zimmerman Tools)
  - `RegistryExplorer.exe` (análisis manual de la colmena)
  - `Timeline Explorer` / `EZViewer`

---

## 2. Metadatos Forenses Extraídos por AmcacheParser
AmcacheParser examina las subclaves de la colmena y genera archivos `.csv` estructurados:

1. **`InventoryApplicationFile` (o tabla `File` en versiones anteriores)**:
   - **Hash SHA-1**: AmCache calcula y almacena un hash criptográfico SHA-1 del binario (calculado sobre los primeros 30 MB o cabecera PE).
   - **Ruta Completa (`LowerCaseLongPath`)**: Ruta absoluta del archivo ejecutable.
   - **Linker Timestamp**: Fecha y hora exacta de compilación extraída del encabezado COFF/PE.
   - **Metadatos de Recursos PE**: `FileDescription`, `ProductName`, `CompanyName`, `OriginalFilename`, `Size`, `BinFileVersion`.
2. **`UnassociatedFileEntries.csv`**:
   - Binarios que no están vinculados a ningún paquete de instalación formal (MSI o entrada en `Add/Remove Programs`).
   - **Foco principal de inspectores forenses**: Aquí caen el 99% de loaders, cheats portátiles y herramientas ejecutadas desde carpetas temporales o escritorios.
3. **`InventoryDriverBinary`**:
   - Información de drivers de kernel (`.sys`) cargados en el sistema (nombres, hashes, certificados de firma).
4. **Marcas de Tiempo de Ciclo de Vida**:
   - `KeyLastWriteTimestamp`: Fecha y hora de cuándo AmCache indexó por primera vez el binario.

---

## 3. Heurísticas y Reglas de Flag en Screenshares (Echo, Paladin, Ocean, Avenge)
- **Flag 1 — Coincidencia de Hash SHA-1**: Cruce automático contra bases de datos en la nube de cheats públicos y privados conocidos.
- **Flag 2 — Presencia en `UnassociatedFileEntries` con Rutas Sospechosas**: Binarios ejecutados desde `AppData`, `Temp`, `Downloads`, discos extraíbles o carpetas ocultas.
- **Flag 3 — Metadatos PE Faltantes o Anómalos**: Ejecutables sin recursos de versión (`CompanyName`, `FileDescription` vacíos) o con nombres de compilador genéricos (MinGW, Nim, PyInstaller).
- **Flag 4 — Inconsistencia de Marcas Temporales**: Ejecutables con `LinkerTimestamp` reciente o en el futuro ("time skew") en comparación con el registro en AmCache.
- **Flag 5 — Colmena Amcache.hve Vacía o Borrada**: Un sistema en uso activo con `Amcache.hve` recién generado o reseteado indica destrucción de evidencia deliberada (*Amcache Purge*).
