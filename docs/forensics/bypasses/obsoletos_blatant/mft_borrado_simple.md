# Bypass Obsoleto / Blatant: Borrado Simple y Timestomping Naive

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Técnicas comúnmente utilizadas en herramientas de bypass públicas o antiguas que generan detección inmediata en escaneos con **MFTECmd** y herramientas de screenshare (Echo, Paladin, Ocean).

---

## 1. Borrado Directo de Archivos (`DeleteFile`, `cmd /c del`, `Remove-Item`)
- **Técnica errónea**: Ejecutar el loader o payload desde `C:\Users\...\AppData` o `C:\Users\...\Downloads` y borrarlo al terminar con `DeleteFileW()`.
- **Por qué falla ante MFTECmd**:
  - En NTFS, borrar un archivo **NO elimina el registro de la MFT**. Únicamente cambia el bit del encabezado de asignación a `InUse = 0` (marcado como libre para reutilización).
  - El registro de 1024 bytes permanece con el nombre original (`WrathLoader.exe`, `cheat.dll`), su tamaño exacto, la ruta completa (mediante `ParentRecordNumber`) y todas sus marcas de tiempo originales intactas.
  - Al ejecutar `MFTECmd.exe -f C:\$MFT --csv .`, el analizador extrae todos los registros con `InUse = 0`. Cualquier ejecutable en carpetas de usuario borrado minutos antes de la revisión es una **detección flagrante (Ban inmediato)**.

---

## 2. Timestomping Básico con `SetFileTime`
- **Técnica errónea**: Cambiar las fechas del archivo para que parezca creado hace 3 años usando la API estándar de Win32 `SetFileTime(hFile, lpCreationTime, ...)`.
- **Por qué falla ante MFTECmd / Timeline Explorer**:
  - `SetFileTime` solo tiene acceso para modificar el atributo `$STANDARD_INFORMATION`.
  - El atributo `$FILE_NAME` es administrado estrictamente por el driver del sistema de archivos (`ntfs.sys`) y retiene la fecha y hora reales en que el archivo fue creado en ese disco.
  - MFTECmd y Timeline Explorer comparan automáticamente `$SI` contra `$FN`. Cuando `$SI.CreationTime` es anterior a `$FN.CreationTime` ("time travel anomaly") o cuando la precisión de `$SI` tiene ceros en los nanosegundos (`2021-05-10 12:00:00.0000000`), el escáner lo resalta en rojo como `Timestomping Detected`.

---

## 3. Detención de Servicios del Sistema o Journaling
- **Técnica errónea**: Intentar deshabilitar el servicio `SysMain`, `EventLog` o detener el USN Journal con comandos como `fsutil usn deletejournal /d C:`.
- **Por qué falla**:
  - Viola la **Regla 1 de Admisión** de este proyecto.
  - Eliminar el USN Journal genera un salto gigantesco en los USN ID y resetea el diario, lo cual es analizado de inmediato por inspectores forenses como evidencia de manipulación maliciosa ("USN Journal Deleted / Tampered").
  - Provoca advertencias críticas en los logs de eventos del sistema de archivos.

---

## 4. Wiping Masivo de MFT con Herramientas Destructivas (e.g. SDelete)
- **Técnica errónea**: Usar herramientas tipo `sdelete -c` o `cipher /w` para sobrescribir el espacio libre de la MFT.
- **Por qué falla**:
  - Genera decenas de miles de entradas en `$LogFile` y `$UsnJrnl` en cuestión de segundos.
  - La actividad de "Disk Sanitization" o "Free Space Wiping" es una de las principales firmas de culpabilidad que buscan Echo y Paladin.
