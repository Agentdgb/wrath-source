# Bypass Obsoleto / Blatant: Borrado Masivo de Prefetch, LNK y Claves de Shell

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Las técnicas más ingenuas y destructivas que delatan culpabilidad automática al ser analizadas con herramientas como PECmd, LECmd y ShellBagsExplorer.

---

## 1. Vaciar la Carpeta `C:\Windows\Prefetch\`
- **Técnica errónea**:
  ```cmd
  del /f /q C:\Windows\Prefetch\*.pf
  ```
- **Por qué falla ante PECmd / WinPrefetchView (Baneo Inmediato)**:
  - En Windows 10/11, la carpeta Prefetch almacena hasta 1024 archivos `.pf` generados por el uso normal del sistema.
  - Al ejecutar PECmd o WinPrefetchView, los revisores comprueban la cantidad de archivos y sus marcas de creación.
  - Si la carpeta contiene apenas 0 o 3 archivos `.pf`, o todos fueron creados hace 10 minutos en un equipo con días de encendido, el revisor marca de inmediato **`Prefetch Cleared / Evidence Tampering`**. Baneo permanente instantáneo.

---

## 2. Dejar Archivos Prefetch de 0 Bytes
- **Técnica errónea**: Crear un archivo falso `LOADER.EXE-XXXXXXXX.pf` con 0 bytes de tamaño y denegar permisos de escritura para que Windows no guarde el prefetch.
- **Por qué falla**:
  - PECmd detecta archivos `.pf` que no poseen la cabecera válida `MAM` (comprimida con LZXPRESS) o `SCCA` (sin comprimir).
  - Un archivo de 0 bytes o corrupto es resaltado automáticamente en color rojo brillante por PECmd como **`Corrupted / Zero-byte Prefetch Tampering`**.

---

## 3. Purgar la Carpeta `Recent` (`del %APPDATA%\...\Recent\*`)
- **Técnica errónea**: Borrar todos los archivos `.lnk` de la carpeta de recientes antes de la revisión.
- **Por qué falla ante LECmd**:
  - Un usuario real que utiliza navegadores, juegos, documentos y carpetas acumula decenas de archivos `.lnk` legítimos.
  - Una carpeta `Recent` vacía en un screenshare es la prueba clásica de que el usuario ejecutó un script de limpieza de pánico.

---

## 4. Borrar Claves de `UserAssist` o `Shellbags`
- **Técnica errónea**: Borrar la clave `HKCU\...\Explorer\UserAssist` o `HKCU\...\Shell\BagMRU`.
- **Por qué falla**:
  - Al borrar la clave, `RegistryExplorer` recupera los registros borrados en el espacio no asignado (slack space).
  - `ShellBagsExplorer` detecta que el árbol de directorios de navegación de Windows Explorer fue reseteado o destruido, emitiendo una alerta crítica de encubrimiento forense.
