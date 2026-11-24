# Bypass Limpio: Alternativas de Inyección Sigilosa al DLL Hijacking

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

El DLL Hijacking basado en colocar archivos en disco en la carpeta del juego está completamente obsoleto y quemado. La arquitectura de Wrath / VANTA sustituye el secuestro de búsqueda por técnicas en memoria que eliminan la presencia de módulos no firmados en el disco y en las listas de módulos del sistema.

---

## 1. Vector Principal: Manual Mapping Desvinculado de PEB (Unlinked Ldr)
*Ubicación en el código:* `Bypasses/injector/ManualMap.h` (`RemoteLoaderStub`).

### Mecanismo:
1. En lugar de forzar a Windows a cargar una DLL desde disco (`LoadLibraryA`), el inyector lee el binario del payload (`WrathPayload.dll`) desde memoria cifrada.
2. Asigna memoria virtual en el proceso destino (`javaw.exe`) con las protecciones de página requeridas.
3. Copia las secciones del PE (`.text`, `.rdata`, `.data`), procesa manualmente la tabla de reubicaciones (`Base Relocations`) y resuelve la tabla de importaciones (`Import Address Table / IAT`).
4. **Invisibilidad ante System Informer y Process Explorer**:
   - El payload **NO se registra en las listas enlazadas del cargador de Windows**:
     - No está en `PEB->Ldr->InLoadOrderModuleList`.
     - No está en `PEB->Ldr->InMemoryOrderModuleList`.
     - No está en `PEB->Ldr->InInitializationOrderModuleList`.
   - Cuando System Informer enumera los módulos del proceso de Minecraft, `WrathPayload.dll` **no existe en la lista de DLLs**.

---

## 2. Inyección Respaldada por Transacción (TxF-backed SEC_IMAGE)
*Ubicación en el código:* `Bypasses/injector/ManualMap.h` (Líneas 20-30).

1. **El Problema de `MEM_PRIVATE`**:
   - Escáneres forenses avanzados detectan inyecciones en memoria buscando páginas ejecutables privadas (`Type = MEM_PRIVATE` con `PAGE_EXECUTE_READWRITE` o `PAGE_EXECUTE_READ`).
2. **Solución Wrath vía TxF**:
   - Se crea una sección de memoria respaldada por una transacción de NTFS (`NtCreateSection` con flag `SEC_IMAGE` utilizando un archivo señuelo legítimo del sistema).
   - Se mapea la vista de la sección (`NtMapViewOfSection`) en el proceso destino.
   - Se cancela la transacción (`RollbackTransaction`).
   - Las páginas de memoria figuran ante `VirtualQueryEx` como `Type = MEM_IMAGE` legítimas, respaldadas aparentemente por un binario de sistema, burlando las heurísticas de detección de páginas privadas ejecutables sin archivo.

---

## 3. Remoción del Hilo Remoto (`HIDE_FROM_DEBUGGER`)
*Ubicación en el código:* `Bypasses/bypass/StealthDefs.h` y `ManualMap.h`.

1. La ejecución del punto de entrada se dispara invocando `NtCreateThreadEx` pasando el atributo no documentado:
   ```cpp
   #define THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER 0x00000004
   ```
2. Esto oculta el hilo de depuradores del sistema y de herramientas forenses que intentan interceptar eventos de creación de hilos en tiempo real.
