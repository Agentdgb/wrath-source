# Detección Forense: Process Hollowing y Vaciado de Imagen en Memoria

## 1. Visión General del Vector
El vaciado de proceso es una técnica de inyección que crea un proceso legítimo en estado suspendido (`CREATE_SUSPENDED`), desmapea la imagen original de su espacio de direcciones, escribe el payload en su base de imagen canónica, ajusta el contexto del hilo principal para que `RIP`/`EIP` apunte al nuevo Entry Point, y reanuda la ejecución. El objetivo es que el proceso figure en la lista de procesos del sistema con un nombre legítimo, ocultando el código real que ejecuta.

---

## 2. Mecanismo Técnico

### Cadena de Llamadas Estándar
1. `CreateProcess(..., CREATE_SUSPENDED, ...)` → proceso crea en estado suspendido.
2. `NtUnmapViewOfSection(hProcess, imageBase)` → desmapea la imagen original desde la base.
3. `VirtualAllocEx(hProcess, imageBase, payloadSize, MEM_RESERVE|MEM_COMMIT, PAGE_EXECUTE_READWRITE)` → reserva en la misma base.
4. `WriteProcessMemory(hProcess, ...)` → escribe cabecera PE y secciones del payload.
5. `SetThreadContext(hThread, ...)` → ajusta `EAX`/`RCX` al nuevo Entry Point.
6. `ResumeThread(hThread)` → reanuda ejecución.

---

## 3. Vectores de Detección Activos

### A. Discrepancia VAD ↔ Archivo en Disco (`PEB->ImageBaseAddress`)
El campo `PEB->ImageBaseAddress` del proceso victima sigue apuntando a la imagen original. Sin embargo, el descriptor VAD (Virtual Address Descriptor) correspondiente a esa región ya no está respaldado por el archivo `.exe` en disco:
* **Tipo de región VAD**: Pasa de `IMAGE` respaldado por el archivo PE a `PRIVATE` / no mapeado por archivo.
* **Detección**: Herramientas como `Volatility3` (plugin `windows.vadinfo`), `Process Hacker → Memory → Type=Private at ImageBase`, o la correlación del volcado de módulos con el hash del archivo en disco detectan la discrepancia en segundos.

### B. Punto de Entrada del Hilo en Espacio No Respaldado por Módulo
Al reanudar el hilo:
* `GetThreadContext.Rcx` apunta a la base del payload.
* `LdrFindEntryForAddress()` falla para esa dirección (no existe entrada en `PEB->Ldr->InLoadOrderModuleList`).
* **Detección**: System Informer, Process Hacker, e inspecciones manuales de inicio de hilo muestran puntos de entrada en regiones `MEM_PRIVATE` sin módulo asociado. Es el indicador más blatant posible de inyección.

### C. Permisos de Región (`PAGE_EXECUTE_READWRITE`)
La región asignada con `VirtualAllocEx` necesita `PAGE_EXECUTE_READWRITE` para recibir las secciones del payload y luego ejecutarlas:
* **Detección**: Cualquier escáner de memoria que busque páginas simultáneamente legibles, escribibles y ejecutables (`MEM_PRIVATE` + `PAGE_EXECUTE_READWRITE`) sobre la base de un proceso estándar lo marca como indicador de inyección crítico (regla YARA universal).

### D. Firma en Event Log y Telemetría (`Event ID 4688` / `Sysmon Event 8`)
* **Event ID 4688** (Process Create) registra el proceso lanzado con `CREATE_SUSPENDED`.
* **Sysmon Event 8** (`CreateRemoteThread`) y **Event 10** (`ProcessAccess`) registran los accesos de escritura y contexto.
* Velociraptor y Chainsaw detectan esta secuencia de eventos en la correlación temporal.

### E. `NtUnmapViewOfSection` en ETW / Syscall Audit
* El uso de `NtUnmapViewOfSection` sobre la propia imagen base desde un proceso externo es extremadamente raro en software legítimo.
* Auditores de llamadas al sistema vía ETW (Event Tracing for Windows) pueden registrar la secuencia `NtUnmapViewOfSection` → `NtAllocateVirtualMemory` → `NtWriteVirtualMemory` como firma de vaciado.
