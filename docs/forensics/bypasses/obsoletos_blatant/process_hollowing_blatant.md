# Técnicas Flagrantes: Process Hollowing, Runas Suspendidas y WriteProcessMemory Manual

## 1. Visión General de Técnicas Descartadas
El vaciado de proceso es una de las técnicas de inyección más documentadas y detectadas del catálogo de adversarios. Su inclusión en cualquier arquitectura de cliente moderno es un error técnico crítico que resulta en detección inmediata por múltiples herramientas usadas en screenshares avanzadas.

---

## 2. Implementaciones Flagrantes

### 1. Vaciado Clásico con `NtUnmapViewOfSection` + `PAGE_EXECUTE_READWRITE`
```cpp
// FLAGRANTE — detectado por pe-sieve, hollows_hunter, Process Hacker
CreateProcess(L"notepad.exe", nullptr, nullptr, nullptr, FALSE,
    CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);

NtUnmapViewOfSection(pi.hProcess, imageBase);  // Desmapeo de imagen original

VirtualAllocEx(pi.hProcess, (LPVOID)imageBase, payloadSize,
    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);  // RWX → indicador crítico

WriteProcessMemory(pi.hProcess, (LPVOID)imageBase, payload, payloadSize, nullptr);

ctx.Rcx = ep; // Entry point del payload
SetThreadContext(pi.hThread, &ctx);
ResumeThread(pi.hThread);
```

**Por Qué es Flagrante**:
* `PAGE_EXECUTE_READWRITE` en la base de imagen del proceso es el indicador #1 de vaciado en cualquier herramienta forense.
* `NtUnmapViewOfSection` sobre la propia imagen del proceso desde un proceso externo genera una secuencia ETW rara con firma estática.
* `pe-sieve` detecta la discrepancia entre el archivo en disco (`notepad.exe`) y el contenido en memoria del proceso.
* `hollows_hunter` reporta explícitamente: `[!] Replaced PE at ImageBase`.

### 2. Process Doppelgänging (TxF + Vaciado)
La variante que usa TxF para escribir el payload en un archivo transaccional y luego crear una sección `SEC_IMAGE`, **revirtiendo la transacción** para que el archivo no persista en disco.

**Por Qué está Degradada**:
* Fue parcheada por Microsoft en Windows 10 1803+ (corrección del flujo `NtCreateProcessEx` con handles de sección transaccionales).
* Process Hacker y Volatility3 detectan la discrepancia `MEM_IMAGE` sin archivo en disco correspondiente (el mapeo original se forzó con la transacción revertida).
* `windows.malfind` la detecta por la ausencia de cabecera PE coherente en memoria respecto a disco.

### 3. Hilo Remoto con `CreateRemoteThread` a `MEM_PRIVATE`
```cpp
// FLAGRANTE — Sysmon Event 8, pe-sieve shellc, bstrings
LPVOID addr = VirtualAllocEx(hTarget, nullptr, shellcodeSize,
    MEM_COMMIT, PAGE_EXECUTE_READWRITE);  // RWX en espacio privado
WriteProcessMemory(hTarget, addr, shellcode, shellcodeSize, nullptr);
CreateRemoteThread(hTarget, nullptr, 0,
    (LPTHREAD_START_ROUTINE)addr, nullptr, 0, nullptr);
```
* `CreateRemoteThread` es auditado por **Sysmon Event 8** con nombre del proceso origen y dirección de inicio del hilo en el proceso destino.
* El punto de entrada del hilo en `MEM_PRIVATE` RWX es el indicador #1 de shellcode en screenshares con `hollows_hunter` o `pe-sieve`.

---

## 3. Tabla de Indicadores Generados

| Técnica | Indicador en Memoria | Indicador en Eventos | Herramienta que lo detecta |
|---|---|---|---|
| Vaciado clásico | `MEM_PRIVATE RWX` en ImageBase | ETW: `NtUnmapViewOfSection` + `NtAllocateVirtualMemory` | pe-sieve, hollows_hunter, PH |
| Doppelgänging | `MEM_IMAGE` sin archivo en disco | Ninguno (stealth parcial) | Volatility3, pe-sieve /shellc |
| `CreateRemoteThread` a shellcode | `MEM_PRIVATE RWX` en heap | Sysmon Event 8 | pe-sieve, bstrings, PH |
| `WriteProcessMemory` manual | Hilo huérfano fuera de módulo | Sysmon Event 10 | hollows_hunter, Volatility |
