# Evasión Limpia: Zero-Recycle, VHD y Ausencia de Interacción con la Papelera

## 1. Principio: Zero-Recycle por Diseño
El vector de la papelera de reciclaje solo existe como superficie forense cuando el usuario interactúa con archivos en el volumen del sistema host (`C:\`). La arquitectura Wrath elimina este vector por diseño:

> *"Ningún archivo del loader, payload o herramienta de sesión reside jamás en el volumen `C:\` del host fuera del contenedor VHD. No hay nada que eliminar desde `C:\`. Por tanto, no hay nada que aparezca en `C:\$Recycle.Bin`."*

---

## 2. Mecanismo: Contenedor VHD con Volumen Aislado

### Montaje de Contenedor sin Letra de Unidad
El entorno de ejecución completo de Wrath vive en un contenedor VHD montado con las banderas de la API `AttachVirtualDisk`:
* `ATTACH_VIRTUAL_DISK_FLAG_NON_PNP (0x40)`: Sin emisión de eventos PnP (`setupapi.dev.log`).
* `ATTACH_VIRTUAL_DISK_FLAG_NO_LOCAL_HOST (0x08)`: Sin exposición al `ShellHWDetection`.
* Sin letra de unidad → sin aparición en el `Explorer`, sin carpeta `$Recycle.Bin` visible.

Resultado: El VHD tiene su propio volumen NTFS con su propia `$Recycle.Bin` aislada. Incluso si se elimina algún archivo interno durante la sesión, el artefacto queda confinado en la imagen VHD, que se encuentra fuera del alcance del MFT del volumen `C:\`.

### Teardown y Desmontaje Limpio
Al concluir la sesión, el VHD se desmonta mediante `DetachVirtualDisk`. La imagen `.vhd` / `.vhdx` se almacena en `C:\System_Backups\Backup_2026.vhd` con atributos `FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM`, y su propia entrada de `$MFT` no apunta a ninguna carpeta del juego ni a ninguna herramienta reconocible.

---

## 3. Prohibición de Interacción con la Papelera del Host

Reglas operativas activas en la sesión Wrath:

| Acción Prohibida | Riesgo Forense |
|---|---|
| Mover archivos a la papelera con `Delete` en `C:\` | Genera `$I<id>` con ruta original absoluta |
| Vaciar la papelera (`Empty Recycle Bin`) | Genera `USN_REASON_FILE_DELETE` masivo + registros MFT marcados `InUse=0` |
| Usar `Shift+Delete` directamente en `C:\` | Genera `USN_REASON_FILE_DELETE` sin papelera pero con entrada MFT residual |
| Colocar archivos del payload en `C:\Users\...\AppData\` y luego eliminarlos | La ruta `AppData` aparece en `$I` → revela nombre del usuario y del archivo |

La única forma segura de eliminar un archivo de sesión es no crearlo en `C:\` desde el principio.

---

## 4. Integración con `wrath::trace::teardown`

La función `restore_prefetch()` en `Bypasses/trace/prefetch.cpp` usa `FILE_DISPOSITION_FLAG_POSIX_SEMANTICS` para eliminar el archivo de Prefetch del loader (si existe) **sin generar entradas en la papelera**:

```cpp
// prefetch.cpp — eliminación atómica sin papelera, sin USN REASON_DELETE convencional
FILE_DISPOSITION_INFO_EX info{};
info.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
SetFileInformationByHandle(hFile, FileDispositionInfoEx, &info, sizeof(info));
```

`POSIX_SEMANTICS` desvincula el archivo del árbol de directorio de forma atómica sin moverlo a la papelera, sin generar el par `$I`/`$R`, y emitiendo un único evento USN `REASON_FILE_DELETE` sin la secuencia de renombrado previa que delataría la interacción con la papelera.
