# Bypass Limpio: Evasión de Ejecución y Existencia de Archivos

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura la ausencia completa de firmas de ejecución y existencia de archivos en el sistema operativo mediante el uso de teardowns forenses a bajo nivel, inyección desvinculada y cero interacción con el shell gráfico de Windows Explorer.

---

## 1. Vector Prefetch: Restauración Atómica y Desvinculación POSIX
*Ubicación en el código:* `Bypasses/trace/prefetch.cpp` (`restore_prefetch`).

1. **El Problema del Prefetch (`C:\Windows\Prefetch\*.pf`)**:
   - Cuando se ejecuta un `.exe` con el servicio `SysMain` activo, Windows crea un archivo `.pf` que contiene el recuento de ejecuciones, marcas de tiempo y la lista de DLLs cargadas.
2. **Solución Wrath**:
   - **Paso 1 — Captura de Snapshot Previo**: Antes de iniciar la sesión de inyección, la suite captura una instantánea exacta del archivo `.pf` del señuelo si existía (bytes binarios, hash SHA-256 y timestamps `$SI`).
   - **Paso 2 — Desvinculación Semántica POSIX (`force_posix_delete`)**:
     Si el archivo `.pf` fue creado de nuevo durante la sesión, se elimina utilizando la API nativa `NtSetInformationFile` con la clase `FileDispositionInformationEx` y el flag:
     ```cpp
     disp.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
     ```
     A diferencia de `DeleteFileW`, la semántica POSIX elimina de forma inmediata el enlace del archivo en la tabla de directorio de NTFS, evitando que el archivo quede bloqueado por `SysMain` y sin dejar residuos de locking.
   - **Paso 3 — Restauración Exacta y Verificación Hash**: Si el señuelo preexistía (e.g. `robocopy.exe`), se reescriben los bytes exactos originales, se restauran los timestamps originales con `SetFileTime` y se valida que el SHA-256 final coincida al 100% con la instantánea previa.

---

## 2. Inmunidad del Payload en Prefetch de Minecraft (`JAVAW.EXE-XXXXXXXX.pf`)
*Ubicación en el código:* `Bypasses/injector/ManualMap.h`.

1. `Prefetch` registra las bibliotecas cargadas en los primeros 10 segundos del proceso mediante intercepción del cargador del sistema operativo (`ntdll!LdrLoadDll`).
2. Al inyectar `WrathPayload.dll` mediante **Manual Mapping puro**:
   - El payload no invoca `LoadLibrary` ni pasa por `LdrLoadDll`.
   - El archivo `JAVAW.EXE-XXXXXXXX.pf` **no incluye `WrathPayload.dll`** en su lista de dependencias DLL inspeccionada por PECmd.

---

## 3. Cero Interacción con Windows Explorer (Inmunidad LNK, UserAssist, Shellbags)
*Regla de diseño de Wrath / VANTA:*

1. **Inmunidad en LNK Files (`Recent\*.lnk`)**:
   - Windows Explorer genera un archivo `.lnk` únicamente cuando el usuario abre o hace doble clic en un archivo desde la interfaz de Explorer.
   - `WrathLoader.exe` se ejecuta de forma programática y desacoplada.
   - En la carpeta `%APPDATA%\Microsoft\Windows\Recent\` **no existe ningún archivo `.lnk`** que apunte a los componentes del cliente.
2. **Inmunidad en UserAssist**:
   - `UserAssist` solo registra ejecuciones lanzadas mediante el shell gráfico (`explorer.exe`).
   - Al ejecutarse sin invocar el shell de Explorer, en la clave `UserAssist\{GUID}\Count` **no se genera ninguna entrada ROT13** del cargador.
3. **Inmunidad en Shellbags (`BagMRU` / `Bags`)**:
   - `Shellbags` solo registra las carpetas que el usuario navegó visualmente en el Explorador de archivos.
   - El usuario nunca abre el Explorador de Windows para explorar la carpeta privada del VHD (`SystemDataCache`).
   - Al analizar el registro con `ShellBagsExplorer.exe`, **no existe ningún registro de navegación hacia los directorios de Wrath**.

---

## 4. Mimetización de Event ID 4688 (Process Creation)
1. El loader se despliega con el nombre y recursos de versión oficiales de utilidades administrativas (`DisplayTunerService.exe` o `BackupManager.exe`).
2. Si el sistema tiene activada la auditoría de procesos, el Evento 4688 registra un proceso utilitario normal, sin argumentos de inyección delatadores en su línea de comandos.
