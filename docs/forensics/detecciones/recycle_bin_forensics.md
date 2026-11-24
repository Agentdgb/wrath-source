# Detección Forense: Papelera de Reciclaje de Windows ($Recycle.Bin / $I / $R)

## 1. Visión General del Vector
Cuando un usuario elimina un archivo en Windows con la combinación de teclas `Delete` (sin `Shift`), el sistema mueve el archivo a la carpeta `C:\$Recycle.Bin\<SID>\` y genera dos artefactos con el mismo identificador secuencial único:

* **`$I<id>.<ext>`**: Archivo de metadatos de 544 bytes. Contiene la ruta original absoluta del archivo, su tamaño en bytes, y la marca de tiempo de eliminación en formato FILETIME (100 ns desde el 1 de enero de 1601 UTC).
* **`$R<id>.<ext>`**: El contenido real del archivo eliminado, renombrado con un identificador opaco.

Incluso después de vaciar la papelera, los registros del `$MFT` y el slack space del `$i30` del directorio `C:\$Recycle.Bin\<SID>` pueden retener evidencia suficiente para reconstruir qué se eliminó, cuándo, y desde qué ruta.

---

## 2. Mecanismo Forense Detallado

### Estructura del Archivo `$I` (544 bytes)
| Offset | Tamaño | Campo |
|---|---|---|
| 0x00 | 8 bytes | Número de versión de header (`0x0000000000000001` para Vista+) |
| 0x08 | 8 bytes | Tamaño del archivo original (en bytes, little-endian) |
| 0x10 | 8 bytes | Timestamp de eliminación en formato FILETIME |
| 0x18 | 4 bytes | Longitud de la ruta original (en caracteres Unicode) |
| 0x1C | 520 bytes | Ruta original absoluta (Unicode null-terminated, máx 260 chars) |

### Proceso de Atribución por SID
Cada SID de usuario tiene su propia subcarpeta dentro de `C:\$Recycle.Bin`:
```
C:\$Recycle.Bin\S-1-5-21-XXXXXXXX-XXXXXXXX-XXXXXXXX-XXXX\
```
Correlacionando el SID con el registro `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList`, el investigador determina qué usuario realizó la eliminación.

---

## 3. Vectores de Detección Activos

### A. Lectura Directa del Directorio `$Recycle.Bin` (Archivos `$I`)
Herramientas como **RBCmd** (`EricZimmermann`), `FTK Imager` y `Autopsy` parsean todos los archivos `$I` del directorio y reconstruyen:
* Nombre original del archivo eliminado.
* Ruta completa de origen.
* Timestamp de eliminación (en UTC).
Incluso si el usuario vació la papelera posteriormente (`Empty Recycle Bin`), los archivos `$I` tienen un ciclo de vida corto tras la eliminación, pero los registros del `$MFT` conservan sus metadatos con `InUse = 0`.

### B. Reconstrucción desde el `$MFT` (con `InUse = 0`)
Al vaciar la papelera, Windows desaloca los registros `$MFT` de los archivos `$I` y `$R`, pero los bytes del registro permanecen en el disco hasta que el clúster sea sobrescrito por nuevos archivos. Herramientas como `MFTECmd` con la flag `--de` (deleted entries) los recuperan íntegramente, mostrando:
* Nombre del archivo `$I<id>` → revela el ID del elemento eliminado.
* Nombre del archivo `$R<id>` → revela el ID del contenido.
* Timestamps `$STANDARD_INFORMATION` y `$FILE_NAME` del momento de creación en la papelera.

### C. `$i30` Slack Space del Directorio de la Papelera
El directorio `C:\$Recycle.Bin\<SID>` mantiene un árbol B+ NTFS (`$INDEX_ALLOCATION`, atributo `$i30`) con entradas de directorio. Las entradas eliminadas permanecen como slack space dentro de los clusters del índice y son recuperables con `INDXRipper` o `MFTECmd -t`.

### D. `$UsnJrnl` (Journal USN)
Cada eliminación, movimiento a la papelera y vaciado de papelera genera entradas secuenciales en el Journal USN:
* `USN_REASON_RENAME_OLD_NAME` / `USN_REASON_RENAME_NEW_NAME`: Cuando el archivo se renombra a `$R<id>`.
* `USN_REASON_FILE_DELETE` + `USN_REASON_CLOSE`: Cuando se vacía la papelera.
El nombre original previo al renombrado queda registrado en las entradas `RENAME_OLD_NAME`.
