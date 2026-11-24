# Ocultación de Cadenas en Memoria JVM y Constant Pool (Análisis jank.systems)

## 1. Visión General del Vector
El artículo *"Hiding strings in java memory"* (Vince Calder / `awrped`, jank.systems) analiza la problemática de las cadenas literales en entornos Java/Minecraft (`javaw.exe`), donde las herramientas de screenshare (Process Hacker, Cheat Engine, bstrings, YARA, MagnetProcessCapture) escanean la memoria RAM en busca de términos delatores (`"aimassist"`, `"autoclicker"`, `"reach"`, URLs de Discord, etc.).

La técnica propuesta busca evitar la persistencia de cadenas completas en la tabla de constantes (`Constant Pool`) y en la memoria del montículo (Heap), recurriendo a la fragmentación en tiempo de compilación y la reconstrucción dinámica mediante `String.join()`, apoyándose en el análisis de escape (*Escape Analysis*) y el reemplazo escalar (*Scalar Replacement*) del compilador JIT C2 de HotSpot.

---

## 2. Mecanismo Técnico Expuesto por jank.systems

### A. Vulnerabilidad de Cadenas Literales y Concatenación Tradicional
* **Literal Estándar**:
  ```java
  System.out.println("aimassist");
  ```
  `javac` almacena `"aimassist"` como una entrada `CONSTANT_Utf8_info` y `CONSTANT_String_info` en el Constant Pool del `.class`. Permanece en el archivo y en la memoria de la JVM de por vida (`String Intern Table`).
* **Concatenación Simple (`+`)**:
  ```java
  String s = "aim" + "ass" + "ist";
  ```
  `javac` optimiza en compilación (*constant folding*) y genera un único literal `"aimassist"` en el Constant Pool.

### B. El Enfoque `String.join()` en Tiempo de Ejecución
* Para evitar el plegado de constantes, Calder propone:
  ```java
  System.out.println(String.join("", "ai", "m", "as", "si", "st"));
  ```
* **Bytecode resultante**:
  ```bytecode
  ldc ""
  bipush 5
  anewarray java/lang/CharSequence
  dup; iconst_0; ldc "ai"; aastore
  dup; iconst_1; ldc "m"; aastore
  dup; iconst_2; ldc "as"; aastore
  ...
  invokestatic java/lang/String.join
  invokevirtual java/io/PrintStream.println
  ```
* En el Constant Pool del archivo `.class` solo existen los fragmentos (`"ai"`, `"m"`, `"as"`, etc.), nunca la palabra completa.

### C. Optimización HotSpot C2 JIT (Escape Analysis & Scalar Replacement)
* En Java 9+ (Compact Strings), `String.join()` reserva un array de bytes (`byte[] value = StringConcatHelper.newArray((int) len)`) y crea `new String(value, coder)`.
* Si el objeto creado no "escapa" del método local (por ejemplo, se evalúa, se procesa o se imprime y no se almacena en variables globales):
  * **HotSpot C2 JIT** activa `PhaseMacroExpand::can_eliminate_allocation()` y `PhaseMacroExpand::scalar_replacement()` (`hotspot/share/opto/macro.cpp`).
  * El compilador JIT **elimina completamente la reserva de memoria en el Heap**.
  * Los bytes individuales se manejan directamente en registros de CPU o en la pila del hilo.
  * Como consecuencia, un volcado de memoria en frío de `javaw.exe` no encuentra la cadena contigua en el montículo de Java.

---

## 3. Vectores de Detección Forense (Cómo lo Cazan los Checkers)

A pesar de su elegancia teórica para variables efímeras locales, esta técnica presenta **tres vectores de detección letales** en una auditoría forense real:

### 1. Desensamblado de Clases y Detección de Patrones en Bytecode
* Si el checker vuelca las clases cargadas en memoria (vía Java Agent, SA-JDI, `DumpJVMClasses`, o inspección del JAR/Fabric Mod en disco):
  * La presencia de cadenas de instrucciones repetitivas:
    `anewarray` → `dup` → `iconst_X` → `ldc <fragment>` → `aastore` → `invokestatic String.join`
    es una **firma heurística evidente de ofuscación de cadenas**.
  * El Constant Pool contiene decenas de fragmentos inusuales de 1 a 3 caracteres agrupados secuencialmente.

### 2. Rotura del Análisis de Escape (Escape Analysis Failure)
* En un cliente de juego o cheat real, los nombres de módulos, configuraciones, estados de renderizado y claves de red **no son efímeros**; deben persistir:
  ```java
  public class Module {
      private String name = String.join("", "Ai", "mA", "ssist"); // ¡ESCAPA!
  }
  ```
* Al asignarse a un campo de instancia o estático, o al pasarse a métodos externos no inlineables:
  * El análisis de escape (`can_eliminate_allocation`) **falla**.
  * HotSpot se ve forzado a reservar el `byte[]` en el Heap de Java (Eden Space).
  * La cadena `"AimAssist"` se materializa como un objeto `java.lang.String` contiguo y persistente en el Heap.
  * Los scanners de memoria (`Process Hacker`, `bstrings`, `Cheat Engine`, `RedLotus`) la encuentran de inmediato.

### 3. Falta de Resistencia frente a Dynamic Instrumentation (JDB / Frida)
* Si el auditor engancha `java.lang.String.join(CharSequence, CharSequence...)` mediante un agente de depuración o Java Attach API, intercepta todos los argumentos y el valor de retorno en el momento de la llamada, reconstruyendo todas las cadenas en texto plano.
