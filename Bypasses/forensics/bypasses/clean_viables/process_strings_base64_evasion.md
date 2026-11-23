# Bypass Limpio: Evasión de Volcados de Proceso, bstrings y Base64Hunter

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass garantiza que un volcado de memoria virtual de `javaw.exe` capturado con Magnet Process Capture y analizado con `bstrings` o `Base64Hunter` devuelva cero coincidencias sospechosas.

---

## 1. Vector Principal: Prohibición Total de Base64 y Ofuscación Criptográfica
*Ubicación en el código:* `Bypasses/core/enc.h` (`EncryptedBuffer`, `StackString`).

1. **La Trampa de Base64**:
   - Base64 **no es cifrado**; es una simple codificación de representación de datos.
   - Herramientas como Base64Hunter decodifican cualquier bloque de base64 automáticamente.
2. **Estándar Wrath**:
   - Prohibido terminantemente el uso de Base64 en cualquier parte del código fuente de Wrath / VANTA.
   - Toda información sensible (nombres de funciones JNI, nombres de módulos, constantes y configuraciones) se procesa en tiempo de compilación mediante `WRATH_STACK_STRING` con el algoritmo de mezcla no lineal de 64 bits (`mix_key`):
     ```cpp
     uint8_t k = static_cast<uint8_t>((Key >> ((i % 8) * 8)) ^ (i * 0x5B + 0x33));
     data[i] = static_cast<char>(str[i] ^ k);
     ```
   - **Propiedades Criptográficas**:
     - Los bytes resultantes en `.rdata` tienen alta entropía binaria y **no utilizan el alfabeto ni el padding (`=`) de Base64**.
     - Base64Hunter ni siquiera detecta los datos cifrados como candidatos válidos para decodificación.

---

## 2. Inmunidad ante `bstrings`: Destrucción Inmediata de Cadenas (`StringScrub.h`)
*Ubicación en el código:* `Bypasses/bypass/StringScrub.h`.

1. **Reconstrucción Efímera en la Pila**:
   - Las cadenas solo se descifran en buffers locales de la pila (`StackString`) durante la llamada exacta a la API del sistema o de la JVM (duración: < 5 microsegundos).
2. **Borrado Seguro Post-Ejecución**:
   - Inmediatamente después de su uso, se llama a `vanta::util::scrubString(s)`:
     ```cpp
     inline void scrubString(std::string& s) noexcept {
         if (!s.empty()) SecureZeroMemory(s.data(), s.size());
         std::string{}.swap(s);
     }
     ```
   - Esto sobrescribe físicamente la memoria con ceros e invalida la capacidad de la cadena mediante `swap()`.
   - Cuando Magnet Process Capture congela el proceso para volcar la memoria, las cadenas ya fueron destruidas de la RAM; `bstrings` no puede extraer lo que físicamente ya no existe.

---

## 3. Mimetización de Nombres de Módulos con el Código Vanilla de Minecraft
*Ubicación en el código:* `Bypasses/strings/mc_names.h`.

1. Para cadenas que deban persistir en el runtime (e.g. campos de clases o identificadores internos):
   - Se utilizan nombres de clases oficiales ofuscadas de Minecraft / Fabric (e.g. `net.minecraft.class_310`, `render_target`, `gl_buffer_cache`).
2. Al analizar con `bstrings`, las pocas cadenas persistentes se confunden al 100% con los cientos de miles de cadenas naturales del motor gráfico y de red del propio juego.
