# Técnicas Flagrantes y Obsoletas de Ocultación de Strings en Java

## 1. Visión General de Métodos Inviables
El intento de ocultar cadenas de texto manipulando el compilador `javac` o las estructuras internas de la JVM sin recurrir a un módulo nativo externo suele generar **anomalías estructurales mayores que las que pretende evitar**, provocando la detección inmediata en revisiones de screenshare y análisis de volcados.

---

## 2. Técnicas Descartadas y Flagrantes

### 1. Fragmentación Ingenua con `String.join()` en Variables Persistentes
* **La Ilusión**: Pensar que usar `String.join("", "ai", "m", "as", "si", "st")` oculta permanentemente la cadena en la memoria de `javaw.exe`.
* **Por Qué es Flagrante**:
  * Funciona únicamente en variables locales que son eliminadas por el análisis de escape (*Scalar Replacement*).
  * En cuanto la cadena se almacena en una variable de clase (`private String moduleName = ...`) o se pasa a una lista de módulos activos, **el análisis de escape falla obligatoriamente**.
  * HotSpot crea el array `byte[]` en el Heap (Eden/Old Generation), dejando el texto completo `"aimassist"` contiguo en RAM, accesible para cualquier escáner de memoria (`Process Hacker`, `Cheat Engine`).
  * En el Constant Pool del archivo `.class`, los fragmentos de 1 a 3 caracteres quedan grabados de forma permanente, creando una firma estática instantánea para decompiladores (`Recaf`, `CFR`, `Javap`).

### 2. Codificación Base64 en Java
* **La Ilusión**: Almacenar cadenas codificadas en Base64 (`"YWltYXNzaXN0"`) y decodificarlas en tiempo de ejecución con `Base64.getDecoder().decode()`.
* **Por Qué es Flagrante**:
  * Herramientas forenses como `Base64Hunter` o `bstrings` buscan patrones de alta entropía con padding `=`, decodificando automáticamente todas las cadenas sospechosas encontradas en el binario o en la memoria.
  * La cadena decodificada se materializa inmediatamente en el Heap de Java como un nuevo objeto `java.lang.String` no cifrado que persiste en memoria.

### 3. Mutación de `String.value` mediante `Unsafe` / Reflexión
* **La Ilusión**: Intentar limpiar la memoria de un `String` accediendo por reflexión o `sun.misc.Unsafe` al campo privado `value` y rellenándolo con ceros:
  ```java
  Field valueField = String.class.getDeclaredField("value");
  valueField.setAccessible(true);
  byte[] val = (byte[]) valueField.get(myString);
  Arrays.fill(val, (byte) 0);
  ```
* **Por Qué es Flagrante**:
  * **Violación de Módulos Java (JEP 396/403)**: A partir de Java 16/17+, el encapsulamiento fuerte de los paquetes internos de la JVM (`java.lang`) prohíbe el acceso reflexivo profundo por defecto, lanzando `IllegalAccessException` a menos que se usen flags explícitas (`--add-opens`), lo cual es una bandera roja inmediata en la línea de comandos de lanzamiento.
  * **Compact Strings (Java 9+)**: `String.value` cambió de `char[]` a `byte[]` con un campo adicional `coder`. Los hacks antiguos asumen `char[]` y generan `ClassCastException` o corrupción de memoria en la JVM.
  * **Corrupción del String Pool**: Si la cadena mutada había sido compartida o internada por la JVM (por ejemplo, vía `String.intern()`), mutar el array subyacente corrompe todas las demás referencias del sistema que apunten a esa cadena, causando errores impredecibles y fallos catastróficos en el juego.

### 4. Ofuscadores de Bytecode con Clases Cifradas en Array de Bytes
* **La Ilusión**: Usar un transformador de bytecode genérico que inserta métodos de descifrado XOR por clase en Java:
  ```java
  private static String decrypt(byte[] b, int key) { ... }
  ```
* **Por Qué es Flagrante**:
  * Los motores forenses de YARA y las reglas de detección de software de trampas en Fabric/Forge buscan específicamente métodos estáticos de descifrado con firmas XOR típicas (`ixor`, `bipush`).
  * Cada invocación deposita las cadenas descifradas en el String Table global de la JVM, permitiendo su volcado integral en una sola pasada.

---

## 3. Tabla Comparativa de Resistencia Forense

| Técnica | Visibilidad en Constant Pool | Visibilidad en Memoria Heap | Resistencia a Decompilador | Resistencia a Scanners de RAM |
|---|---|---|---|---|
| **Literal Directo** | `"aimassist"` visible | Visible de por vida | Nula (0%) | Nula (0%) |
| **`String.join` (jank.systems)** | Fragmentos visibles | Visible si la variable escapa | Baja (patrón evidente) | Baja (cadena contigua en Heap) |
| **Base64 en Java** | Hash Base64 visible | Visible tras decodificar | Nula (auto-decodificable) | Nula (detectable por regex) |
| **Reflexión / Unsafe** | Texto plano visible | Corrupción de JVM | Nula | Inestable / Flagrante |
| **Wrath Native (`enc.h`)** | Inexistente (Cero Java) | Cero (cifrado en pila + scrub) | Total (100% Nativo C++20) | Total (100% Inmune a Heap Dumps) |
