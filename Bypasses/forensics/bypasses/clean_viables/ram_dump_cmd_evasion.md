# Bypass Limpio: Evasión de Volcados de RAM y Recuperación de Comandos

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que un volcado de memoria física completa (`ram.raw`) analizado con HxD o Volatility no revele comandos de inyección, rutas sospechosas ni títulos de ventana asociados al cliente.

---

## 1. Vector Principal: Cero Dependencia de Consolas (`cmd.exe` / `conhost.exe`)
*Regla de diseño de Wrath / VANTA:*

1. **Subsystem Windows Nativo**:
   - `WrathLoader.exe` se compila bajo el subsistema gráfico de Windows (`/SUBSYSTEM:WINDOWS`), no como aplicación de consola (`/SUBSYSTEM:CONSOLE`).
   - El cargador utiliza su propia interfaz gráfica basada en DirectX 11 / FreeType.
2. **Cero Creación de `conhost.exe`**:
   - Al no invocar `cmd.exe` ni inicializar un host de consola interactivo:
     - En la memoria física **no existe ninguna cadena de título** `"Administrator: Command Prompt"` ni `"Command Prompt"` vinculada al cliente.
     - No existen búferes de entrada/salida de consola en la RAM.
   - Si el revisor ejecuta `WinPmem64`, busca en HxD el valor hexadecimal `41 64 6D 69 6E 69 73 74 72 61 74 6F 72 3A 20 43 6F 6D 6D 61 6E 64...` y analiza los bloques adyacentes, **únicamente encontrará comandos legítimos del sistema operativo** (e.g. comandos de red o mantenimiento que el usuario haya hecho en el pasado), sin rastro alguno de Wrath.

---

## 2. Purgado de Memoria en Espera (Standby List Purge vía NTAPI)
Si durante sesiones de desarrollo o configuración previa se utilizó la terminal de comandos:

1. **El Problema**: Las páginas de procesos cerrados persisten en la lista de espera (*Standby List*) de la RAM física.
2. **Solución Limpia sin Reinicio**:
   - Se invoca la función nativa no documentada de Windows `NtSetSystemInformation` con la clase `SystemMemoryListInformation` (orden `MemoryPurgeStandbyList` = 4):
     ```cpp
     // Requiere SeProfileSingleProcessPrivilege o SeIncreaseQuotaPrivilege
     typedef enum _SYSTEM_MEMORY_LIST_COMMAND {
         MemoryCaptureAccessedBits,
         MemoryCaptureAndResetAccessedBits,
         MemoryEmptyWorkingSets,
         MemoryFlushModifiedList,
         MemoryPurgeStandbyList,
         MemoryPurgePriority0List,
         MemoryFlushModifiedListWithFlushWait
     } SYSTEM_MEMORY_LIST_COMMAND;

     SYSTEM_MEMORY_LIST_COMMAND cmd = MemoryPurgeStandbyList;
     NtSetSystemInformation(SystemMemoryListInformation, &cmd, sizeof(cmd));
     ```
   - Esta llamada (la misma que utiliza la herramienta oficial `RAMMap` de Microsoft Sysinternals) libera y sobrescribe de forma inmediata las páginas en espera de procesos finalizados, purgando los residuos de memoria física de consolas cerradas.

---

## 3. String Scrubbing de Parámetros de Proceso
*Ubicación en el código:* `Bypasses/bypass/StringScrub.h`.

1. Cualquier argumento o ruta en memoria temporal dentro del loader se sobrescribe con `SecureZeroMemory` antes de llamar a `ExitProcess`.
2. Al liberarse el proceso, sus páginas no contienen fragmentos legibles de rutas en la RAM.
