# Bypass Limpio: Evasión de Velociraptor y Escaneos YARA en Memoria

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este estándar asegura que las consultas VQL y los escaneos de procesos con YARA en vivo ejecutados por Velociraptor no detecten firmas de memoria, páginas privadas sospechosas ni conexiones anómalas.

---

## 1. Vector Principal: Neutralización de YARA vía Cifrado Efímero de Cadenas
*Ubicación en el código:* `Bypasses/core/enc.h` y `Bypasses/bypass/StringScrub.h`.

1. **Inmunidad ante Reglas YARA Basadas en Cadenas**:
   - Las reglas YARA de Velociraptor buscan cadenas de texto (`strings: $a = "..." wide nocase`).
   - En Wrath / VANTA, **ninguna cadena sensible existe estáticamente en memoria**:
     - Cada cadena se cifra en tiempo de compilación con una clave XOR de 64 bits derivada de `BUILD_SALT` (`enc.h`).
     - Al ejecutarse, la cadena se reconstruye en la pila local (`StackString`) únicamente durante los microsegundos necesarios para su consumo.
     - Inmediatamente después, se invoca `SecureZeroMemory` mediante `StringScrub.h`, sobrescribiendo la memoria con ceros.
   - Si Velociraptor ejecuta un scan YARA sobre `javaw.exe`, las cadenas no están presentes en la memoria; la consulta VQL devuelve **cero coincidencias**.

---

## 2. Evasión de Caza de Código Inyectado (VAD Cleanliness)
*Ubicación en el código:* `Bypasses/injector/ManualMap.h`.

1. **Cero Páginas `PAGE_EXECUTE_READWRITE` (RWX)**:
   - Wrath jamás deja regiones ejecutables con permisos de lectura/escritura/ejecución (el indicador número uno en VQL para `Windows.Memory.InjectedCode`).
   - Se aplican estrictamente los permisos legítimos por sección:
     - `.text`: `PAGE_EXECUTE_READ`
     - `.rdata`: `PAGE_READONLY`
     - `.data`: `PAGE_READWRITE`
2. **Respaldo de Imagen Legítima vía Transacción NTFS (TxF)**:
   - En lugar de páginas `MEM_PRIVATE` anónimas, el inyector utiliza `NtCreateSection` con el flag `SEC_IMAGE` respaldado por una transacción de un binario nativo de Windows (TxF), cancelando luego la transacción (`RollbackTransaction`).
   - Las páginas de memoria figuran ante la consulta VAD de Velociraptor como `MEM_IMAGE` legítimas, eludiendo la heurística de inyección de código sin respaldo.

---

## 3. Cero Conexiones de Red Sospechosas en Sockets Volátiles
1. `WrathPayload.dll` dentro de `javaw.exe` no mantiene conexiones persistentes hacia servidores de trampas durante el juego.
2. Toda la actividad de paquetes de red se realiza interceptando los canales del cliente Fabric oficial (JNI/Netty), por lo que ante la consulta `Windows.Network.Netstat` de Velociraptor, el único tráfico visible es la conexión legítima hacia el servidor de Minecraft.

---

## 4. Desenganche Completo en Unload (`CIERRE_SEGURO.md`)
*Ubicación en el código:* `wrath/docs/CIERRE_SEGURO.md`.

1. Si el usuario activa la descarga / autodestrucción:
   - Se restauran los métodos Java originales en la JVM antes de desvincular cachés.
   - Se restauran los trampolines de MinHook a sus bytes originales.
   - Se liberan las páginas asignadas y se realiza zeroing previo.
   - Si Velociraptor corre después del Unload, el proceso de Minecraft es idéntico a una sesión vanilla.
