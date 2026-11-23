# Evasión Limpia: Module Overloading SEC_IMAGE y Micro-Stomping en .text (Wrath / VANTA)

## 1. Principio: Memoria Respaldada sin Registro LDR

La regla fundamental de esta arquitectura:

> *"El payload nunca se aloja en `MEM_PRIVATE` mediante `VirtualAlloc` ni se registra en las listas oficiales del LDR con `LoadLibraryW`. Toda la imagen de Wrath reside en una vista `MEM_IMAGE` respaldada por archivo mediante `NtCreateSection(SEC_IMAGE)`, haciéndola invisible a las comprobaciones basadas en `PEB->Ldr` y normal ante escaneos de memoria heurísticos."*

Esto elimina el flag más contundente de las herramientas de análisis de screenshare (Process Hacker / Process Explorer / Echo / Paladin): la presencia de una DLL extraña no autorizada en el árbol de módulos del proceso de Minecraft (`javaw.exe`).

---

## 2. Arquitectura de Dos Fases

### Fase A: Micro-Stomping Staging en DLL Víctima (~0x180 bytes)
En lugar de un stomping masivo que destruye megabytes de una biblioteca del sistema operativo (lo cual delata inmediatamente una anomalía de módulo reemplazado en PE-sieve), se utiliza un micro-stub PIC de solo ~0x180 bytes ubicado en el margen de seguridad (`VirtualSize / 8 > 0x1000`) de la cola de la sección `.text` de una biblioteca prescindible (`dbghelp.dll`, `version.dll`, `uxtheme.dll` o `wintrust.dll`):
- El stub está aislado en la sección `.wstub` del inyector (`stub.cpp`), compilado con `/GS- /Gs- /EHs-c- /GR-`.
- No requiere dependencias del CRT ni funciones estáticas del sistema.
- Resuelve `ntdll.dll` caminando el PEB de forma manual (`GetProcW`).
- Emplea `LdrGetDllHandle` y `LdrGetProcedureAddress` (`NtApi`) para resolver de forma segura funciones del sistema, cubriendo forwards y redirecciones de ApiSet.

### Fase B: Mapeo SEC_IMAGE y Bootstrap
1. El inyector crea una sección de tipo `SEC_IMAGE` apuntando al payload (`display_service64.dll`) con permisos `PAGE_EXECUTE_READ`.
2. Mapea la vista en el espacio de memoria de `javaw.exe` con `NtMapViewOfSection`.
3. La llamada a `NtCreateThreadEx` (con flag `0x4` / `THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER`) salta a `StubInit`:
   - Registra la tabla invertida de excepciones (`.pdata`) con `RtlAddFunctionTable` para permitir que el runtime de C++ y los bloques `try/catch` funcionen sin crashear el proceso.
   - Invoca los callbacks TLS de inicialización si existen.
   - Llama a `DllMain(DLL_PROCESS_ATTACH)`.
   - Resuelve el export `WrathShutdown` y ejecuta `WrathBootstrap`.
4. El inyector restaura los bytes originales de la cola de `.text` de la DLL víctima y limpia la memoria del stub.

---

## 3. Matriz de Contención Forense

| Vector Forense | Resultado con Module Overloading | Motivo Técnico |
|---|---|---|
| **Pestaña Modules (Process Hacker / VMMAP)** | **100% INVISIBLE (PASS)** | No se invoca `LdrpLoadDll`. La DLL no existe en `PEB->Ldr`. |
| **Escáneres Whitelist (Echo, Paladin, Ocean)** | **100% INVISIBLE (PASS)** | `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE)` y `EnumProcessModules` dan vacío. |
| **PE-sieve (`/shellc` Shellcode Scanner)** | **LIMPIO (PASS)** | Memoria asignada como `MEM_IMAGE`, no como memoria privada anónima (`MEM_PRIVATE`). |
| **PE-sieve (Comparación diff de disco)** | **LIMPIO (PASS)** | El contenido de la memoria coincide byte a byte con el archivo PE mapeado. |
| **Integridad de DLL víctima (`dbghelp.dll`)** | **PRESERVADA (PASS)** | Se restaura el código original de `.text` tras el disparo del bootstrap. |
| **Forense de memoria en frío (Post-Unload)** | **LIMPIO (PASS)** | `OverloadUnload` ejecuta sanitización con ceros (`WriteProcessMemory`) antes de desmapear. |
| **Moneta / Kernel VAD Walkers** | **ALERTA LEVE** | Detecta discrepancia de imagen mapeada fuera del LDR (*Unregistered mapped module*). |

---

## 4. Teardown y Cierre Seguro (`OverloadUnload`)

La secuencia de descarga remota garantiza cero corrupción en HotSpot / JVM:
1. **Llamada a `WrathShutdown`:**
   - Detiene workers y activa la bandera `g_transformerPassthrough = true`.
   - Espera el drenado de hilos propios que ejecutan `vm->DetachCurrentThread()`.
   - Desengancha hooks nativos (`jnihook` / MinHook).
   - Libera el entorno JVMTI (`DisposeEnvironment`).
2. **Llamada a `StubTeardown`:**
   - Invoca `DllMain(DLL_PROCESS_DETACH)`.
   - Despacha callbacks TLS en orden inverso.
   - Elimina la tabla invertida de excepciones mediante `RtlDeleteFunctionTable`.
3. **Destrucción de Memoria:**
   - Se cambia la protección a `PAGE_READWRITE` y se sobreescribe toda la región con ceros.
   - Se desmapea la sección con `NtUnmapViewOfSection`.
   - Se restaura el `.text` original de la DLL víctima en `javaw.exe`.
   - Se cierra el handle de la sección `hSection`.
