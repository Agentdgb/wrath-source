# Bypass Obsoleto / Blatant: Borrado Simple y Residuos en $INDEX_ALLOCATION

## Nivel: RECHAZADO / OBSOLETO / BLATANT

El error forense más común en NTFS: confiar en que eliminar un archivo limpia su rastro del directorio padre.

---

## 1. Borrado Directo del Loader desde Carpetas de Usuario (`Desktop`, `Downloads`, `Temp`)
- **Técnica errónea**:
  Colocar `WrathLoader.exe` o `cheat.exe` en `C:\Users\Admin\Desktop` o en `%TEMP%`, ejecutarlo y luego presionar `Shift + Supr` o ejecutar `del /f loader.exe`.
- **Por qué falla ante INDXRipper (Baneo Inmediato)**:
  - Borrar el archivo solo modifica el registro del archivo en la MFT (`InUse = 0`).
  - En la carpeta padre (`Desktop` o `Temp`), el bloque `$INDEX_ALLOCATION` (árbol B-Tree) **conserva la entrada del archivo en el slack space**.
  - Al ejecutar el analista:
    ```cmd
    INDXRipper.exe //./C: --deleted-dirs output.csv
    ```
    INDXRipper carvea el bloque de 4096 bytes del directorio y extrae una fila completa con:
    - Nombre: `loader.exe`
    - Parent Directory: `C:\Users\Admin\Desktop`
    - File Size: `1,250,432 bytes`
    - $FN Timestamps: Fecha de hace 15 minutos.
  - La prueba forense de que el ejecutable estuvo en esa carpeta es irrefutable.

---

## 2. Borrar la Carpeta Contenedora Entera
- **Técnica errónea**: Crear una carpeta `C:\Users\Admin\AppData\Local\Temp\CheatFolder`, colocar los archivos dentro y luego borrar la carpeta completa (`rmdir /s /q CheatFolder`).
- **Por qué falla**:
  - Al borrar la carpeta completa, NTFS desasigna los clusters que contenían el bloque `$INDEX_ALLOCATION` de `CheatFolder`.
  - Esos clusters pasan a espacio no asignado (unallocated space) en el volumen `C:`.
  - El modificador `--deleted-dirs` de INDXRipper fue diseñado específicamente para escanear todo el espacio no asignado del volumen, localizando los bloques de índice de carpetas eliminadas y recuperando todos los archivos que alguna vez residieron dentro de ellas.

---

## 3. Uso de Herramientas Destructivas de Borrado (SDelete)
- **Técnica errónea**: Usar `sdelete -z` o `sdelete -p` para borrar el archivo.
- **Por qué falla**:
  - SDelete sobrescribe el contenido de los clusters de datos del archivo y su registro en la MFT.
  - **Sin embargo, SDelete NO limpia el slack space de los nodos B-Tree del directorio padre (`$i30`)**.
  - El nombre del archivo y sus marcas `$FN` continúan siendo recuperables mediante INDXRipper, al mismo tiempo que la actividad de SDelete genera miles de transacciones de alerta en `$LogFile`.
