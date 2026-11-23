# Detección Forense: Análisis del Atributo $i30 en NTFS (INDXRipper)

## 1. Identificación y Alcance de la Tecnología
- **Artefacto**: Atributo de índice `$i30` de directorios NTFS:
  - `$INDEX_ROOT` (Atributo `0x90`): Contiene la raíz del árbol de indexación dentro del registro MFT de la carpeta.
  - `$INDEX_ALLOCATION` (Atributo `0xA0`): Bloques de asignación externos (nodos de 4096 bytes) que forman el árbol B-Tree cuando una carpeta contiene múltiples archivos.
  - `$BITMAP` (Atributo `0xB0`): Mapa de bits que indica qué bloques de índice del árbol B-Tree están actualmente en uso o libres.
- **Herramientas de Análisis**:
  - `INDXRipper.exe` (desarrollado por Harel Segev):
    ```cmd
    INDXRipper.exe //./C: --deleted-dirs output.csv
    ```
  - `X-Ways Forensics` / `Autopsy` (módulos de carving de `$INDEX_ALLOCATION`).
  - `MFTECmd` (Eric Zimmerman).

---

## 2. Funcionamiento Forense y Persistencia tras el Borrado
1. **Estructura de una Entrada `$i30`**:
   - Cada nodo del índice de un directorio contiene entradas estructuradas para cada archivo o subcarpeta hijo:
     - Nombre del archivo/carpeta.
     - Número de registro MFT correspondiente (`MFT Record Number` + `Sequence Number`).
     - Tamaño físico y lógico del archivo.
     - Atributos del archivo (Hidden, Read-Only, System).
     - **Timestamps MACB del atributo `$FILE_NAME`** (Modified, Accessed, Changed, Born).
2. **Supervivencia a la Eliminación**:
   - Cuando un usuario borra un archivo con `DeleteFile` o `Shift + Supr`:
     - El registro del archivo en la MFT se marca como libre (`InUse = 0`).
     - En el directorio padre, la entrada del archivo se elimina lógicamente del árbol B-Tree o se marca como inactiva en el `$BITMAP`.
     - **Sin embargo, los bytes de la entrada permanecen intactos en el slack space del bloque `$INDEX_ALLOCATION`**.
   - Los nodos de 4096 bytes rara vez se compactan o sobrescriben de inmediato si no se crean cientos de nuevos archivos en esa misma carpeta.
3. **Carving con INDXRipper**:
   - INDXRipper inspecciona los bloques `$INDEX_ALLOCATION` a nivel de sectores físicos `//./C:`.
   - Carvea y reconstruye todas las entradas de archivos eliminados que residieron en esa carpeta, exportando a CSV el nombre original del binario, su tamaño exacto y sus marcas de tiempo `$FN`.

---

## 3. Firmas Buscadas en Screenshares
- Presencia de ejecutables de inyectores o loaders borrados de `%TEMP%`, `Downloads`, `Desktop` o carpetas de usuario.
- Scripts de limpieza (`clean.bat`, `bypass.ps1`) borrados minutos antes de la revisión pero recuperados desde el `$i30` de la carpeta temporal.
- Coincidencia temporal entre los timestamps `$FN` de la entrada `$i30` y la sesión de juego revisada.
