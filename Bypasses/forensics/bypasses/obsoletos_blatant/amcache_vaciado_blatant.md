# Bypass Obsoleto / Blatant: Borrado Forzado de Amcache.hve y Deshabilitación de Telemetría

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Métodos destructivos que activan detección inmediata al ser analizados con `AmcacheParser`, `RegistryExplorer` o herramientas automáticas de screenshare.

---

## 1. Borrado Forzado del Archivo `Amcache.hve`
- **Técnica errónea**: Forzar la eliminación de `C:\Windows\AppCompat\Programs\Amcache.hve` mediante herramientas de desbloqueo de archivos (Unlocker), scripts de inicio o desde entornos WinPE.
- **Por qué falla ante AmcacheParser (Baneo Directo)**:
  - `Amcache.hve` es una colmena esencial del sistema operativo.
  - Al forzar su borrado, Windows recrea una colmena nueva de apenas 32 KB prácticamente vacía al reiniciar o reconectar el servicio.
  - Al ejecutar:
    ```cmd
    AmcacheParser.exe -f "C:\Windows\AppCompat\Programs\Amcache.hve" --csv "output"
    ```
    Los inspectores de screenshare verifican el tamaño de la colmena y la cantidad de entradas históricas.
  - Un archivo `Amcache.hve` con tamaño mínimo o con marcas de creación de hace unos minutos en un sistema operativo que lleva meses instalado es clasificado como **`[CRITICAL] AmCache Wiped / Evidence Destruction` (Ban permanente)**.

---

## 2. Deshabilitar Tareas Programadas o Servicios de Compatibilidad
- **Técnica errónea**:
  ```cmd
  schtasks /change /tn "\Microsoft\Windows\Application Experience\ProgramDataUpdater" /disable
  sc config DiagTrack start= disabled
  sc config PcaSvc start= disabled
  ```
- **Por qué falla**:
  - **Violación Directa de la Regla 1**: Servicios y tareas de diagnóstico deben permanecer intactos.
  - Los scripts de inspección de Echo, Paladin y Ocean auditan el estado de las tareas de `Application Experience`. Si `ProgramDataUpdater` está deshabilitada o los servicios `PcaSvc` / `DiagTrack` están detenidos, se marca automáticamente como bandera roja por manipulación del entorno forense.

---

## 3. Edición Naive de Claves con RegistryExplorer / RegEdit
- **Técnica errónea**: Intentar montar la colmena `Amcache.hve` y borrar manualmente las subclaves del ejecutable con `RegDeleteKey`.
- **Por qué falla**:
  - `RegistryExplorer` y `rla.exe` exponen las celdas desasignadas en el espacio slack (ver `detecciones/registry_explorer_rla.md`).
  - Los hashes SHA-1 y las rutas de los archivos eliminados continúan legibles en el slack space de la colmena.
  - Se altera el `LastWriteTime` de la clave raíz, delatando manipulación deliberada.
