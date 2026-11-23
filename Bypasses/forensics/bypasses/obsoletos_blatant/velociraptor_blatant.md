# Bypass Obsoleto / Blatant: Páginas Privadas RWX y Sabotaje de Velociraptor

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Prácticas vulnerables frente a consultas VQL y escaneos de memoria en vivo ejecutados por **Velociraptor**.

---

## 1. Inyección de Shellcode en Páginas `MEM_PRIVATE` con Permisos `PAGE_EXECUTE_READWRITE` (RWX)
- **Técnica errónea**: Asignar memoria en el proceso con `VirtualAllocEx(hProcess, NULL, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE)`, copiar el payload y crear un hilo remoto.
- **Por qué falla ante Velociraptor (Detección Inmediata)**:
  - Los artefactos VQL de caza en memoria (`Windows.Memory.InjectedCode` y `Windows.Detection.ProcessHollowing`) escanean los descriptores VAD del proceso buscando específicamente anomalías de protección.
  - Una región de memoria con permisos de escritura y ejecución (`RWX`) que no esté respaldada por una sección de archivo (`MEM_PRIVATE`) es la firma clásica número 1 de inyección de código.
  - Velociraptor la marca en segundos como amenaza crítica.

---

## 2. Dejar Cadenas de Trampas en Texto Plano en Memoria
- **Técnica errónea**: No cifrar cadenas o dejar búferes con nombres de módulos (`Killaura`, `Aimbot`, `Bypass`) en la memoria de `javaw.exe`.
- **Por qué falla ante YARA VQL**:
  - Un analista puede ejecutar en vivo la consulta:
    ```sql
    SELECT * FROM yara(accessor="process", files=format("/%d", Pid), rules='rule cheat { strings: $a = "killaura" nocase condition: $a }')
    ```
  - YARA lee los bytes virtuales del proceso y extrae la cadena junto con el contexto, revelando la presencia del cliente sin necesidad de tocar el disco.

---

## 3. Intentar Matar el Agente de Velociraptor o Modificar Permisos con `icacls`
- **Técnica errónea**: Forzar la detención de `velociraptor.exe` o quitarle permisos de administrador mediante comandos `icacls`.
- **Por qué falla**:
  - La pérdida de telemetría en tiempo real o el fallo en la ejecución del agente genera una alerta inmediata en la consola del analista por **Bloqueo / Sabotaje de la Herramienta de Auditoría (Auto-Ban)**.
