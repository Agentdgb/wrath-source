# Bypass Limpio: Inyección Programática y Cero Uso de Editores GUI

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura que los procesos críticos del sistema (`explorer.exe`, `ctfmon.exe`, `taskhostw.exe`, `dwm.exe`) no contengan ningún residuo del patrón `) (0x`, garantizando que un volcado de memoria no revele manipulación de procesos.

---

## 1. Vector Principal: Cero Uso de Editores de Memoria Interactivos
*Regla de arquitectura de Wrath / VANTA:*

1. **Manipulación Programática Pura (Zero-GUI Editor)**:
   - Wrath / VANTA nunca realiza modificaciones de memoria abriendo ventanas de Process Hacker, Cheat Engine o System Informer.
   - Toda la inyección, resolución de importaciones y hookeo de métodos se realiza programáticamente mediante código compilado nativo C++20 (`ManualMap.h` / `NtInjector.cpp`).
2. **Resultado en la Memoria del Sistema**:
   - Al no abrirse la ventana gráfica del editor de memoria de System Informer:
     - No se generan títulos de ventana con rangos de memoria.
     - Cero interacción con los búferes de texto de `ctfmon.exe`.
     - Cero cadenas en el compositor de escritorio `dwm.exe`.
   - Si el revisor de screenshare genera el volcado de memoria (`Create dump file`) sobre `explorer.exe`, `ctfmon.exe`, `taskhostw.exe` o `dwm.exe` y busca en Notepad el patrón `) (0x`:
     - La búsqueda devuelve **cero resultados**.
     - No existe ningún PID ni rango hexadecimal delatador.

---

## 2. Aislamiento de la Interfaz del Loader (DirectX 11 Standalone)
*Ubicación en el código:* `wrath/loader/`.

1. `WrathLoader.exe` no utiliza controles de ventana Win32 estándar (`EDIT`, `STATIC`, `LISTBOX`) que interactúan de forma predeterminada con los subsistemas de accesibilidad y UI Automation de Windows.
2. La interfaz de usuario del cargador se dibuja directamente en un swap chain de **DirectX 11 con renderizado de fuentes por FreeType en memoria**:
   - Los textos y opciones de la interfaz no se exponen al subsistema CTF / IME de `ctfmon.exe`.
   - No se envían cadenas de memoria al portapapeles ni a la cola de mensajes del shell de Explorer.

---

## 3. Desvinculación de Hilos Remotos con Ocultamiento de Depurador
1. Al disparar la inicialización del payload dentro de `javaw.exe`, la rutina utiliza `NtCreateThreadEx` con el flag `THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER (0x04)`.
2. No se registran eventos de depuración que puedan ser interceptados por herramientas de monitoreo de procesos.
