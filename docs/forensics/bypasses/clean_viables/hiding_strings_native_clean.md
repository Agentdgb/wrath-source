# Evasión Limpia de Cadenas en Memoria y JVM (Arquitectura Wrath / VANTA)

## 1. Filosofía de Aislamiento de Memoria Nativa
El análisis de jank.systems demuestra las limitaciones inherentes del entorno gestionado de Java: cualquier técnica en bytecode (`String.join`, reflexión o fragmentación) es vulnerable si el objeto escapa al Heap o si se inspecciona el Constant Pool.

En la arquitectura **Wrath / VANTA**, se adopta una regla estricta:
> *"El Heap de Java y los archivos de clase (`.class`) nunca deben contener cadenas de control, nombres de módulos ni firmas de la herramienta. Toda la lógica y datos sensibles residen exclusivamente en espacio nativo de C++20 con cifrado y saneado inmediato."*

---

## 2. Implementación Técnica en Wrath

### A. Cifrado Polimórfico en Tiempo de Compilación (`Bypasses/core/enc.h`)
En lugar de fragmentar cadenas en texto plano dentro del Constant Pool, Wrath cifra las cadenas en tiempo de compilación utilizando metaprogramación de plantillas C++20 con una sal única por compilación (`build_salt.h`):

```cpp
// Bypasses/core/enc.h
#pragma once
#include <array>
#include <string_view>
#include "build_salt.h"

namespace wrath::enc {

template <size_t N, uint32_t Seed>
class StackString {
private:
    std::array<char, N> data_{};

    static constexpr uint8_t get_key(size_t index) {
        return static_cast<uint8_t>((Seed ^ (index * 0x5D)) + BUILD_SALT_LOW);
    }

public:
    constexpr StackString(const char(&str)[N]) {
        for (size_t i = 0; i < N; ++i) {
            data_[i] = str[i] ^ get_key(i);
        }
    }

    __forceinline void decrypt(char* dest) const {
        for (size_t i = 0; i < N; ++i) {
            dest[i] = data_[i] ^ get_key(i);
        }
    }

    __forceinline void scrub(char* dest) const {
        RtlSecureZeroMemory(dest, N);
    }
};

} // namespace wrath::enc

#define WRATH_STACK_STRING(str) \
    []() { \
        constexpr auto s = wrath::enc::StackString<sizeof(str), __COUNTER__ + 0xDEADBEEF>(str); \
        return s; \
    }()
```

### B. Ciclo de Vida y Saneado Atómico en Memoria (`StringScrub.h`)
Cuando una cadena nativa requiere ser evaluada (por ejemplo, resolver un export o comunicarse con el renderizador DX11), se descifra localmente en la pila y se destruye en el mismo bloque:

```cpp
{
    char resolved_name[64];
    auto enc_str = WRATH_STACK_STRING("TargetInterface");
    enc_str.decrypt(resolved_name);

    // Operación atómica de uso
    PerformAction(resolved_name);

    // Destrucción inmediata del texto plano
    enc_str.scrub(resolved_name);
}
// En este punto, la pila contiene solo ceros.
```

### C. Protocolo JNI por Identificadores Numéricos (Zero Java Strings)
Para comunicar el hook de Fabric en Java con la DLL nativa inyectada (`WrathPayload.dll`), **nunca se transmiten cadenas de texto a través de la JNI**.
* **Diseño Vulnerable**:
  ```java
  // ¡PROHIBIDO! Genera objetos String en el Heap de Java
  nativeBridge.toggleModule("AimAssist", true);
  ```
* **Diseño Wrath (Enums Numéricos y Vanilla Hash)**:
  ```java
  // Wrath: Comunicación puramente numérica sin Strings
  nativeBridge.dispatch(0x4E01, 1); // 0x4E01 = Opcode interno Combat-1
  ```
* En el Heap de Java solo existen tipos primitivos (`int`, `long`, `boolean`). Ningún inspector de memoria JVM (`VisualVM`, `JConsole`, `Cheat Engine` sobre `java.lang.String`) puede interceptar nombres de módulos.

### D. Vanilla Spoofing en Strings Públicas (`mc_names.h`)
Si por requerimientos del motor gráfico o de depuración una cadena debe residir en memoria de la JVM, se utilizan exclusivamente identificadores idénticos a los del juego base (`mc_names.h`):
* `net.minecraft.client.render.BufferBuilder`
* `org.lwjgl.opengl.GL11`
* `com.mojang.blaze3d.systems.RenderSystem`

Cualquier volcado de memoria cruzado interpretará estas cadenas como objetos internos del cliente vainilla.
