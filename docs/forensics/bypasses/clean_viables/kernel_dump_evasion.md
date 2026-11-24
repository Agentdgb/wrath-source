# Bypass Limpio: Evasión de Live Kernel Dump y RedLotus

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que si un analista genera un Live Kernel Dump en caliente y lo procesa con `RedLotus` o herramientas similares, no encuentre cadenas reconocibles, comandos sospechosos ni artefactos de inyección en la memoria volcada.

---

## 1. Vector Principal: Cifrado en Tiempo de Compilación (`enc.h`)
*Ubicación en el código:* `Bypasses/core/enc.h` (`EncryptedBuffer`, `StackString`).

1. **El Problema de los Escaneos de Memoria**:
   - RedLotus y los analizadores de dumps ejecutan búsquedas automatizadas de cadenas (`strings`) sobre el archivo `.dmp`.
   - Cualquier cadena en texto plano presente en los binarios (nombres de funciones, rutas, mensajes de error) queda expuesta en el volcado.
2. **Solución Wrath**:
   - Todas las cadenas del cliente y loader se encapsulan en estructuras `StackString` con cifrado XOR polimórfico en tiempo de compilación alimentado por el salt criptográfico aleatorio `BUILD_SALT`:
     ```cpp
     auto str = WRATH_STACK_STRING("DisplayTunerService");
     ```
   - Las cadenas **no existen en texto plano en la sección `.rdata` ni `.data`**.
   - Solo se descifran en la pila de ejecución en el instante exacto en que se consumen, eliminando el vector de detección de RedLotus.

---

## 2. Limpieza Inmediata de Memoria (`StringScrub.h`)
*Ubicación en el código:* `Bypasses/bypass/StringScrub.h` (`scrubString`, `scrubStringVector`).

1. **Borrado Seguro de Buffers Transitorios**:
   - En cuanto una cadena o estructura temporal cumple su propósito, se invoca `SecureZeroMemory`:
     ```cpp
     inline void scrubString(std::string& s) noexcept {
         if (!s.empty()) SecureZeroMemory(s.data(), s.size());
         std::string{}.swap(s);
     }
     ```
   - Esto sobrescribe los bytes de memoria con ceros reales, evitando que el compilador optimice la llamada y garantizando que los buffers desasignados no queden flotando en el pool de memoria del proceso.

---

## 3. Cero LOLBins y Cero Intérpretes de Comandos
*Regla de diseño de Wrath / VANTA:*

1. **Inmunidad ante Regex de RedLotus**:
   - RedLotus busca específicamente: `powershell`, `iex`, `rundll32.exe`, `regsvr32.exe`, `forfiles.exe`, `wmic.exe`, `reg delete`.
   - WrathLoader es un binario C++ nativo que realiza todas las operaciones mediante llamadas directas a la API del kernel (NTAPI).
   - En la memoria del sistema operativo **no existe ningún registro de línea de comandos ni buffers de consola** que contengan estas palabras clave.

---

## 4. Cero Objetos y Drivers de Kernel
1. Al no usar controladores `.sys` ni mappers:
   - No se crean estructuras `DEVICE_OBJECT` ni `DRIVER_OBJECT`.
   - No se asigna memoria en `NonPagedPool` que contenga pool tags sospechosos de kernel reconocibles por WinDbg.
   - El volcado del kernel se mantiene completamente libre de anomalías a nivel Ring 0.
