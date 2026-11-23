# Bypass Obsoleto / Blatant: Inyección RWX y Desvinculación de Procesos (DKOM)

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Las técnicas más ingenuas y quemadas que garantizan detección inmediata al analizar volcados de memoria con **Volatility 3**.

---

## 1. Inyección Directa en `MEM_PRIVATE` con `PAGE_EXECUTE_READWRITE` (RWX)
- **Técnica errónea**:
  Llamar a `VirtualAllocEx(hProcess, NULL, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE)` y escribir la DLL con cabeceras `MZ` en la memoria del juego.
- **Por qué falla ante `windows.malfind.Malfind` (Baneo Inmediato)**:
  - `malfind` está programado con una regla heurística fundamental:
    *Buscar cualquier descriptor VAD de tipo `MEM_PRIVATE` con permisos de ejecución*.
  - En cuanto ejecuta:
    ```bash
    python vol.py -f memdump.mem windows.malfind.Malfind --pid <PID>
    ```
    Volatility imprime en pantalla:
    - Offset exacto de la región en memoria.
    - Tipo: `PAGE_EXECUTE_READWRITE`.
    - Desensamblado de los primeros opcodes (`4D 5A` / cabecera DOS o shellcode de inyección).
  - Esta salida es la prueba de cargo número 1 en informática forense de memoria.

---

## 2. Ocultamiento de Procesos mediante DKOM (Desvincular `ActiveProcessLinks`)
- **Técnica errónea**: Usar un driver de kernel para modificar los punteros `Flink` y `Blink` de la lista doblemente enlazada `ActiveProcessLinks` en la estructura `EPROCESS` del loader.
- **Por qué falla ante `windows.psscan.PsScan`**:
  - `ActiveProcessLinks` es la lista que usa el Administrador de Tareas y `tasklist`.
  - Sin embargo, `PsScan` de Volatility **no utiliza listas enlazadas**. Escanea físicamente los pools de memoria buscando la signatura y el tag del objeto `EPROCESS` (`Proc` / `PoolTag`).
  - Al encontrar un `EPROCESS` válido en memoria que no está registrado en `ActiveProcessLinks`, Volatility emite de inmediato la alerta:
    **`Hidden Process Detected (DKOM Unlinked)`**. Baneo automático por rootkit activo.

---

## 3. Uso de Mutexes con Nombres Genéricos de Trampas
- **Técnica errónea**:
  Crear objetos de sincronización con nombres como:
  `CreateMutexA(NULL, TRUE, "WrathClientMutex_v2")` o `"CheatLoaderActive"`.
- **Por qué falla ante `windows.handles.Handles`**:
  - Al ejecutar:
    ```bash
    python vol.py -f memdump.mem windows.handles.Handles --pid <PID>
    ```
    Volatility lista todos los nombres de objetos creados por el proceso.
  - El revisor busca con `grep -i "cheat\|loader\|inject\|wrath"` y encuentra el objeto delatador con su tipo `Mutant` o `Event`.
