# Técnicas Flagrantes: Manipulación de la Papelera y Borrado Ingenuo de Archivos

## 1. Comportamientos de Pánico Quemados

### 1. Mover el Cheat/Loader a la Papelera y Vaciarla
* **Comportamiento**: Presionar `Delete` sobre la carpeta del inyector para moverla a `C:\$Recycle.Bin\<SID>` y luego hacer clic derecho → "Empty Recycle Bin".
* **Huellas Generadas**:
  1. Archivo `$I<id>.<ext>` creado en `C:\$Recycle.Bin\<SID>\` con la **ruta completa absoluta del cheat** (`C:\Users\...\loader.exe`), su tamaño original y el timestamp de eliminación.
  2. Archivo `$R<id>.<ext>` con el contenido renombrado del archivo.
  3. Al vaciar: registro `USN_REASON_FILE_DELETE` + `USN_REASON_RENAME_OLD_NAME` / `RENAME_NEW_NAME` en `$UsnJrnl` exponiendo el nombre original.
  4. `$MFT` de los archivos `$I` / `$R` queda marcado `InUse = 0` pero **no se sobreescribe de inmediato**. `MFTECmd --de` los recupera con nombre, ruta y timestamps.

### 2. `Shift+Delete` Directo (Sin Papelera)
* **Comportamiento**: Eliminar el archivo omitiendo la papelera con `Shift+Delete`.
* **Huellas Generadas**:
  * Registro `$MFT` del archivo marcado `InUse = 0` con nombre y ruta completa.
  * Entrada `USN_REASON_FILE_DELETE` en `$UsnJrnl` con el nombre del archivo y su `FileReferenceNumber`.
  * Entrada residual en el `$i30` del directorio padre (B-Tree NTFS): el nombre del archivo permanece en el slack space del índice.
  * Ninguna de estas huellas desaparece hasta que el clúster es físicamente sobrescrito por nuevos datos.

### 3. Herramientas de "Limpieza" de Papelera (`CCleaner`, `BleachBit`)
* **Comportamiento**: Usar CCleaner o BleachBit para vaciar la papelera y limpiar archivos temporales.
* **Huellas Generadas**:
  * El proceso `CCleaner.exe` o `BleachBit.exe` figura en Prefetch, BAM, ShimCache y AmCache con timestamp contemporáneo a la citación de screenshare.
  * La ejecución de herramientas de limpieza inmediatamente antes o durante una sesión forense es **considerada en sí misma evidencia de intent de destrucción de pruebas** en las normativas de las principales comunidades de screenshare.
  * `CCleaner.exe` genera un registro de auditoría propio (`ccleaner.ini`, `ccleaner64.log`) que puede ser recuperado.

### 4. Uso de `sdelete` o `eraser` para Sobrescritura Segura
* **Comportamiento**: Intentar usar `sdelete.exe` (Sysinternals) o Eraser para sobrescribir el archivo con datos aleatorios antes de eliminarlo.
* **Huellas Generadas**:
  * `sdelete.exe` figura en Prefetch y BAM con timestamp.
  * El propio `sdelete` **renombra el archivo a una cadena aleatoria antes de eliminarlo** (para ocultar el nombre), lo que genera una secuencia `RENAME_OLD_NAME` / `RENAME_NEW_NAME` en `$UsnJrnl` que revela que existió un renombrado sospechoso seguido de eliminación.
  * El patrón de sobrescritura (múltiples pasadas `0x00`/`0xFF`) es detectable en el slack space residual del MFT si el clúster no fue reutilizado.

---

## 2. Regla de Oro

> *La única forma de que la papelera de reciclaje no revele información es **nunca escribir el archivo en `C:\` en primer lugar**. Cualquier intento de limpiar retroactivamente deja una huella mayor que la que pretende eliminar.*
