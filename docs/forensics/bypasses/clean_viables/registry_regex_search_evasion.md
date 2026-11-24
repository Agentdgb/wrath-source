# Bypass Limpio: Evasión de Búsquedas por Regex en el Registro

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que una búsqueda global con expresiones regulares (`^([a-zA-Z]:\\.+)\\?$`) a través de todas las colmenas del registro no devuelva ninguna ruta perteneciente a ejecutables, DLLs o carpetas del cliente.

---

## 1. Vector Principal: Aislamiento por Montaje VHD Sin Letra de Unidad
*Ubicación en el código:* `Bypasses/tools/setup_vhd_environment.ps1`.

1. **La Trampa de las Letras de Unidad**:
   - Las expresiones regulares de los analistas buscan rutas absolutas comenzando con letras de unidad: `^([a-zA-Z]:\\...)`.
2. **Solución Wrath**:
   - El entorno VHD se monta mediante la API nativa de `virtdisk.dll` (`AttachVirtualDisk`) utilizando el flag:
     ```cpp
     ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER (0x00000002)
     ```
   - La partición interna del VHD se asigna a un **NTFS Volume Mount Point** sin asignar ninguna letra de unidad (`D:`, `E:`, etc.).
   - Al no existir letra de unidad y residir dentro del punto de montaje desacoplado, **en las colmenas del registro de Windows (`NTUSER.DAT`, `SYSTEM`, `SOFTWARE`) no existe ningún valor que contenga una ruta absoluta hacia `WrathLoader.exe` ni `WrathPayload.dll`**.
   - Al ejecutar la regex en Registry Explorer, la consulta simplemente no tiene coincidencias con los componentes de Wrath.

---

## 2. Inmunidad Total ante Filtros de DLLs (`^([a-zA-Z]:\\.+\.dll)\\?$`)
*Ubicación en el código:* `Bypasses/injector/ManualMap.h`.

1. Los analistas filtran por rutas que terminen en `.dll` para identificar módulos inyectados o bibliotecas huérfanas.
2. `WrathPayload.dll` se inyecta mediante **Manual Mapping en memoria**:
   - No se utiliza `AppInit_DLLs`.
   - No se utiliza `KnownDLLs`.
   - No se registra ningún módulo en el registro de Windows.
   - En ninguna colmena del registro existe un valor de tipo cadena o binario que contenga una ruta a una DLL de Wrath.

---

## 3. Cero Persistencia en Claves `Run` / `RunOnce`
*Regla de diseño de Wrath / VANTA:*

1. Wrath es una herramienta de inyección en memoria bajo demanda; jamás escribe claves de inicio automático en `Run` o `RunOnce`.
2. Las claves de persistencia de `HKLM` y `HKCU` permanecen inmaculadas, superando cualquier filtrado por auto-starts.

---

## 4. Inmunidad en `MuiCache` y Diálogos de Archivos
1. **`MuiCache`**:
   - Windows solo genera una entrada en `MuiCache` cuando un ejecutable es iniciado a través de las APIs gráficas del shell de Explorer (`ShellExecuteEx`).
   - El loader se inicia de forma desacoplada sin registrar cadenas descriptivas en el shell de Windows.
2. **Diálogos de Archivos (`ComDlg32`)**:
   - Al no utilizar `GetOpenFileName` ni `GetSaveFileName`, no se registran rutas en `OpenSavePidlMRU` ni `LastVisitedPidlMRU`.

---

## 5. Anti-Slack Space Scrubbing en BAM (`bam.cpp`)
*Ubicación en el código:* `Bypasses/trace/bam.cpp` (`delete_value_anti_slack`).

1. Si la ruta del binario señuelo llegó a ser registrada en `BAM`:
   - En lugar de simplemente borrar el valor, `delete_value_anti_slack` **sobrescribe físicamente la celda del registro** con la ruta de un binario oficial del sistema (`\Device\HarddiskVolume3\Windows\System32\svchost.exe`).
   - Posteriormente se fuerza `RegFlushKey`.
   - Si la regex busca incluso en las celdas desasignadas (slack space), la cadena hallada corresponde a `svchost.exe` en `System32`, clasificándose como un proceso nativo y seguro de Windows.
