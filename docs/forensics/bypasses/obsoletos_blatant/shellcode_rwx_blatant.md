# Técnicas Flagrantes: Shellcode RWX, CreateRemoteThread a MEM_PRIVATE y Hooks Detectables

## 1. Visión General
Las técnicas de inyección que producen regiones `MEM_PRIVATE` ejecutables o hilos cuyo punto de entrada cae fuera del espacio de módulos listados en `PEB->Ldr` son detectadas en segundos por cualquier herramienta de screenshare avanzada. Se documentan aquí como referencia de qué **no hacer**.

---

## 2. Implementaciones Flagrantes

### 1. Allocación RWX + WriteProcessMemory + CreateRemoteThread
```cpp
// FLAGRANTE — Detectado por pe-sieve, hollows_hunter, PH, Sysmon Event 8
LPVOID pRemote = VirtualAllocEx(hJava, nullptr, payloadSize,
    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

WriteProcessMemory(hJava, pRemote, payload, payloadSize, nullptr);

HANDLE hThread = CreateRemoteThread(hJava, nullptr, 0,
    (LPTHREAD_START_ROUTINE)pRemote, nullptr, 0, nullptr);
```

**Indicadores generados en `javaw.exe`**:
* Región `MEM_PRIVATE`, `PAGE_EXECUTE_READWRITE` visible en `Properties → Memory`.
* Hilo con `StartAddress = pRemote` que cae en espacio `MEM_PRIVATE`.
* `pe-sieve /shellc` detecta la región y vuelca el contenido.
* Sysmon Event 8 (`CreateRemoteThread`) con origen del proceso inyector y destino `javaw.exe`.

### 2. VirtualProtect para Cambiar Permisos Post-Escritura
```cpp
// Intento de "stealth" RWX → RX post-escritura (también flagrante)
LPVOID p = VirtualAllocEx(hJava, nullptr, size, MEM_COMMIT, PAGE_READWRITE);
WriteProcessMemory(hJava, p, payload, size, nullptr);
// Cambiar a PAGE_EXECUTE_READ post-escritura
VirtualProtectEx(hJava, p, size, PAGE_EXECUTE_READ, &oldProt);
```

**Por Qué Sigue Siendo Flagrante**:
* La región sigue siendo `MEM_PRIVATE` aunque ya no sea `PAGE_EXECUTE_READWRITE`.
* El historial de protección (`PAGE_READWRITE` → `PAGE_EXECUTE_READ`) en el `VAD` del kernel es reconocible.
* `pe-sieve` y `hollows_hunter` detectan regiones `MEM_PRIVATE` con `PAGE_EXECUTE_READ` sin módulo asociado exactamente igual que las RWX.

### 3. VMT Hooks (Virtual Method Table Patching)
```cpp
// FLAGRANTE — detectable por pe-sieve /iat y comparación de módulo en disco
LPVOID* vtable = *(LPVOID**)object;
DWORD oldProt;
VirtualProtect(&vtable[targetIndex], sizeof(LPVOID), PAGE_EXECUTE_READWRITE, &oldProt);
vtable[targetIndex] = (LPVOID)hookFunction;  // hookFunction en MEM_PRIVATE
VirtualProtect(&vtable[targetIndex], sizeof(LPVOID), oldProt, &oldProt);
```

**Indicadores**:
* El puntero en la VMT apunta a `hookFunction` que reside en `MEM_PRIVATE`.
* `pe-sieve` compara el módulo en memoria con el archivo en disco byte a byte → detecta la modificación de la VMT.
* `Process Hacker → Memory → Find String` puede encontrar los fragmentos del gancho si no están cifrados.

### 4. Trampoline Hooks (Sobrescritura de Prólogo de Función)
```cpp
// FLAGRANTE — detectable por pe-sieve /shellc
BYTE tramp[] = { 0x48, 0xB8,                 // mov rax, imm64
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // dirección hook
                 0xFF, 0xE0 };                // jmp rax

*(ULONG64*)(tramp + 2) = (ULONG64)hookAddress;  // hookAddress en MEM_PRIVATE

DWORD oldProt;
VirtualProtect(targetFunc, sizeof(tramp), PAGE_EXECUTE_READWRITE, &oldProt);
memcpy(targetFunc, tramp, sizeof(tramp));
VirtualProtect(targetFunc, sizeof(tramp), oldProt, &oldProt);
```

**Indicadores**:
* Los primeros 12 bytes del prólogo de la función hookeada en memoria difieren de los del archivo PE en disco.
* `pe-sieve` vuelca la discrepancia en el reporte de módulos.
* `hookAddress` en `MEM_PRIVATE` RWX es detectado como shellcode independientemente.

---

## 3. Tabla de Detección Rápida

| Técnica | `pe-sieve` | `hollows_hunter` | Process Hacker | Sysmon |
|---|---|---|---|---|
| `MEM_PRIVATE RWX` + CRT | Shellcode dump | Shellcode report | Memory tab | Event 8 |
| `MEM_PRIVATE RX` (post-RWX) | Shellcode dump | Shellcode report | Memory tab | — |
| VMT Hook a `MEM_PRIVATE` | Module diff | — | Stack trace | — |
| Trampoline en módulo legítimo | Module diff | Hooks report | — | — |
