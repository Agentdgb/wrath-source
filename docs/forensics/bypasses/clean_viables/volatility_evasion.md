# Bypass Limpio: Evasión de Volatility 3 (Malfind, PsScan, Handles)

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que si un analista captura la RAM física completa y la procesa con Volatility 3, los plugins más rigurosos (`malfind`, `psscan`, `pstree`, `handles`) devuelvan resultados 100% limpios.

---

## 1. Vector Principal: Evasión de `windows.malfind.Malfind`
*Ubicación en el código:* `Bypasses/injector/ManualMap.h`.

1. **Cómo Razona `malfind`**:
   - `malfind` itera sobre el árbol `_MMVAD` de cada proceso.
   - Solo emite alertas si encuentra páginas con `Vad.Flags.Protection` que incluyan ejecución (`EXECUTE`), y cuyo nodo VAD **NO esté asociado a una estructura `FILE_OBJECT`** (es decir, memoria privada `MEM_PRIVATE`).
2. **Solución Wrath mediante TxF `SEC_IMAGE`**:
   - El inyector crea una sección transaccional respaldada por archivo utilizando la API nativa de Windows (`NtCreateSection` con `SEC_IMAGE` sobre un binario legítimo de Windows dentro de una transacción NTFS).
   - Se mapea la sección en `javaw.exe` y se hace rollback de la transacción (`RollbackTransaction`).
   - **Resultado en el Kernel**: En la estructura `_MMVAD`, el tipo de página se registra como **`MEM_IMAGE`** con un puntero a archivo válido.
   - `malfind` ignora por diseño las páginas `MEM_IMAGE` porque de lo contrario reportaría millones de falsos positivos en todas las DLLs legítimas de Windows y Java.
3. **Erradicación de Cabeceras PE (`MZ` / `PE`)**:
   - Inmediatamente después del mapeo y resolución de imports/relocs, se sobrescriben los primeros 4096 bytes del módulo (cabecera DOS, NT Headers y Section Headers) con ceros mediante `RtlSecureZeroMemory`.
   - Si `malfind` llegara a inspeccionar la página, no encuentra el encabezado `0x5A4D` (`MZ`).

---

## 2. Inmunidad ante `windows.psscan.PsScan` y `windows.pstree.PsTree`
*Regla de diseño de Wrath / VANTA:*

1. **Cero Manipulación DKOM de `ActiveProcessLinks`**:
   - Wrath **nunca desvincula estructuras `EPROCESS` en el kernel**.
   - Los trucos antiguos de rootkits que desvinculan la lista enlazada son atrapados al 100% por `psscan` al escanear los pools de memoria.
2. **Ciclo de Vida Limpio**:
   - `WrathLoader.exe` se ejecuta como un proceso legítimo.
   - Al terminar la inyección, invoca `ExitProcess(0)`.
   - El kernel de Windows retira ordenadamente la estructura `EPROCESS` de las tablas y listas del sistema sin dejar residuos huérfanos.
   - En `windows.pstree`, `javaw.exe` figura con su jerarquía de proceso normal (hijo del launcher de Minecraft).

---

## 3. Mimetización de Handles y Mutexes (`StealthDefs.h`)
*Ubicación en el código:* `Bypasses/bypass/StealthDefs.h`.

1. **El Problema de `windows.handles.Handles`**:
   - Volatility lista todos los nombres de semáforos, mutexes y eventos creados en memoria.
2. **Solución Wrath**:
   - Para la sincronización inter-procesos se utiliza un nombre de mutex derivado del PID del proceso que emula a la perfección un objeto de sincronización interno de la máquina virtual Java:
     ```cpp
     inline std::wstring sentinelMutexNameForPid(DWORD pid) {
         return L"Local\\jvm_pid" + std::to_wstring(pid) + L"_signal_sem";
     }
     ```
   - Además, las secciones compartidas utilizan identificadores nativos del runtime de Java (`kHeartbeatMagic = 0x4A565354` // `'JVST'`).
   - Al ejecutar `windows.handles.Handles --pid <PID>`, el analista ve un semáforo estándar de JVM, idéntico a los cientos de objetos internos del runtime.

---

## 4. Inmunidad en `windows.cmdline.CmdLine`
1. Al analizar `javaw.exe` con `windows.cmdline.CmdLine`, la línea de comandos extraída de `PEB->ProcessParameters->CommandLine` contiene exclusivamente los argumentos originales de lanzamiento de Minecraft (`net.minecraft.client.main.Main`).
2. El payload en memoria no altera la línea de comandos del proceso anfitrión.
