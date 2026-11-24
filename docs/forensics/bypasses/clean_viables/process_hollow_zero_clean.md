# Evasión Limpia: Arquitectura Zero-Hollow, SEC_IMAGE y Hilo Respaldado (Wrath / VANTA)

## 1. Principio: Zero-Hollow por Diseño de Arquitectura
El vaciado de proceso es **arquitectónicamente imposible en Wrath** porque la cadena de inyección no recurre en ningún paso a:
* `NtUnmapViewOfSection` sobre la imagen base de ningún proceso.
* `VirtualAllocEx` con `PAGE_EXECUTE_READWRITE`.
* `SetThreadContext` con modificación del Entry Point del hilo principal del proceso huésped.

La filosofía de Wrath es **no crear procesos víctima**: el payload vive dentro de `javaw.exe`, un proceso pre-existente con módulos legítimos. La inyección se realiza exclusivamente mediante mapeo de sección `SEC_IMAGE` respaldado por TxF.

---

## 2. Mecanismo de Inyección con SEC_IMAGE + TxF

### Vínculo a `Bypasses/injector/ManualMap.h`
La implementación completa reside en [`ManualMap.h`](file:///c:/Sources/Projects/DirectXColorRenderer/DirectXCore/Bypasses/injector/ManualMap.h). El flujo:

```cpp
// 1. Crear transacción NTFS (modo TxF)
HANDLE hTx = CreateTransaction(nullptr, nullptr, 0, 0, 0, 0, L"WrSec");

// 2. Abrir el archivo .dll con contexto transaccional (jamás llega a disco)
HANDLE hFileTx = CreateFileTransactedW(
    L"\\\\?\\C:\\...\\display_service64.dll",
    GENERIC_READ | GENERIC_WRITE,
    0, nullptr, OPEN_ALWAYS,
    FILE_ATTRIBUTE_NORMAL, nullptr,
    hTx, nullptr, nullptr
);

// 3. Escribir el payload cifrado en el archivo transaccional
WriteFile(hFileTx, payload_buf, payload_size, ...);

// 4. Crear sección SEC_IMAGE desde el archivo transaccional
HANDLE hSection = nullptr;
NtCreateSection(&hSection, SECTION_MAP_EXECUTE | SECTION_MAP_READ,
    nullptr, nullptr, PAGE_EXECUTE_READ, SEC_IMAGE, hFileTx);

// 5. Mapear la sección en el proceso destino (javaw.exe)
// La región aparece como MEM_IMAGE, PAGE_EXECUTE_READ
// — NO como MEM_PRIVATE, NO con PAGE_EXECUTE_READWRITE
NtMapViewOfSection(hSection, hTarget, &base, 0, 0, nullptr,
    &viewSize, ViewUnmap, 0, PAGE_EXECUTE_READ);

// 6. Revertir la transacción: el archivo jamás se escribe en disco
RollbackTransaction(hTx);
CloseHandle(hTx);
```

**Resultado forense**:
* La región en `javaw.exe` figura como `Type = MEM_IMAGE`, `Protect = PAGE_EXECUTE_READ`.
* No existe ningún archivo PE en disco que se pueda correlacionar con la sección (la transacción fue revertida).
* `pe-sieve` encuentra la discrepancia sección-en-disco vs sección-en-memoria, pero la región **no es MEM_PRIVATE** → no activa la heurística primaria de shellcode/vaciado.

---

## 3. Hilo de Entrada Respaldado (`THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER`)

El hilo que ejecuta el payload se crea con `NtCreateThreadEx`:

```cpp
NtCreateThreadEx(
    &hThread,
    THREAD_ALL_ACCESS,
    nullptr,
    hTarget,
    (LPTHREAD_START_ROUTINE)resolvedEntryPoint,
    lpParam,
    THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER,  // Anti-debug
    0, 0, 0,
    nullptr
);
```

* `THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER` previene que la API Win32 `CreateRemoteThread` sea visible para depuradores que usen `DebugActiveProcess`.
* El punto de entrada `resolvedEntryPoint` apunta a una dirección dentro de la sección `MEM_IMAGE` mapeada, no a una región `MEM_PRIVATE`. `LdrFindEntryForAddress` puede no devolver un módulo válido (porque la transacción fue revertida), pero la región no es `MEM_PRIVATE`.

---

## 4. Prohibición Total de Process Hollowing

La técnica de vaciado está prohibida en Wrath por cuatro razones técnicas acumulativas:

| Indicador de Vaciado | ¿Lo activa Wrath? | Razón |
|---|---|---|
| `NtUnmapViewOfSection` sobre ImageBase | **No** | Wrath no crea ni vacía procesos víctima. |
| `MEM_PRIVATE` con `PAGE_EXECUTE_*` | **No** | Toda memoria ejecutable es `MEM_IMAGE`. |
| Hilo huérfano fuera de módulo | **No** | Entry point en región `MEM_IMAGE`. |
| `NtWriteVirtualMemory` a ImageBase | **No** | La escritura ocurre solo en `hFileTx` (TxF). |
| Discrepancia VAD ↔ disco | Parcial | TxF revierte → no hay archivo en disco que comparar, pero el tipo de región es correcto. |
