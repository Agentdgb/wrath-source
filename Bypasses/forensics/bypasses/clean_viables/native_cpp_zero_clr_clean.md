# Bypass Limpio: Arquitectura C++20 Nativa y Cero Dependencia de CLR

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass elimina al 100% la totalidad de artefactos de CLR y .NET Runtime mediante una arquitectura compilada estrictamente en código nativo (C++20).

---

## 1. Vector Principal: Cero Uso de C# / .NET (Native C++20 Architecture)
*Regla de arquitectura de Wrath / VANTA:*

1. **Compilación Nativa Pura**:
   - `WrathLoader.exe` y `WrathPayload.dll` están programados íntegramente en C++20 estándar (`/std:c++20`) y compilados con MSVC en código máquina nativo x64.
   - **Cero Dependencia de Bibliotecas Gestionadas**:
     - No importa ni carga `mscoree.dll`.
     - No inicializa `clr.dll`, `clrjit.dll` ni `coreclr.dll`.
     - No requiere el runtime de .NET Framework ni .NET Desktop Runtime.
2. **Impacto en los Registros del Sistema**:
   - **En `Application.evtx`**:
     - Al filtrar por `Source = ".NET Runtime"`, no existe ninguna entrada generada por Wrath. Cero Eventos 1026 ni excepciones de assemblies.
   - **En `DotNETRuntime` Log**:
     - Cero registros de carga de ensamblados o compilaciones JIT asociadas al cliente.
   - **En Fusion Log (`HKLM\SOFTWARE\Microsoft\Fusion`)**:
     - Cero registros de enlace de ensamblados gestionados.
   - **En `%LOCALAPPDATA%\Microsoft\CLR_v4.0\UsageLogs`**:
     - No se genera ningún archivo `.log` correspondiente al loader de Wrath.
   - **En Prefetch (`PECmd.exe`)**:
     - La lista de DLLs mapeadas por el loader de Wrath no contiene librerías del Common Language Runtime; solo librerías del sistema Win32/NTAPI (`ntdll.dll`, `kernel32.dll`, `d3d11.dll`, `user32.dll`).

---

## 2. Payload Directo en Java Virtual Machine (JNI Nativo)
*Ubicación en el código:* `wrath/client/`.

1. En Minecraft Fabric (Java), muchos cheats rudimentarios intentan usar shims en C# o puentes inter-proceso que dejan trazas en .NET.
2. `WrathPayload.dll` se comunica con el motor del juego utilizando la **Java Native Interface (JNI)** nativa oficial de la JVM (`jvm.dll`):
   - Invocación de `GetEnv` para obtener la interfaz `JNIEnv*`.
   - Modificación directa de estructuras de clases y métodos de la JVM en C++ puro.
   - MinHook en código máquina nativo sobre el swapchain de OpenGL / DirectX.
3. No se utiliza ningún componente .NET dentro del espacio de memoria de `javaw.exe`.
