# Bypass Obsoleto / Blatant: Edición Manual de Memoria en Process Hacker

## Nivel: RECHAZADO / OBSOLETO / BLATANT

El método más ingenuo de edición de memoria en caliente, ampliamente difundido en tutoriales públicos, que resulta en baneo inmediato ante revisiones forenses de screenshare.

---

## 1. Abrir la Pestaña "Memory" en Process Hacker / System Informer para Modificar Bytes
- **Técnica errónea**:
  Abrir Process Hacker durante o antes de una partida, buscar el proceso de Minecraft (`javaw.exe`) o de un servicio, abrir la pestaña "Memory", hacer doble clic en una región de memoria y modificar bytes a mano (e.g., cambiar instrucciones `nop`, modificar valores flotantes de reach, o pegar shellcode).
- **Por qué falla ante los revisores (Baneo Inmediato)**:
  - La ventana interactiva del editor de memoria de System Informer genera una cadena de título con el formato:
    ```
    (PID) (0xInicio - 0xFin)
    ```
  - Este texto se propaga a los buffers de accesibilidad y gestión de ventanas de los procesos `ctfmon.exe`, `explorer.exe`, `taskhostw.exe` y `dwm.exe`.
  - El revisor genera el dump de `explorer.exe` o `ctfmon.exe` con dos clics en System Informer, abre el dump en Notepad y busca:
    ```
    ) (0x
    ```
  - Notepad resalta inmediatamente la coincidencia exacta:
    `(2804) (0x5a6beac000 - 0x5ac6beb0000)`
  - El revisor comprueba a qué proceso pertenece el PID `2804` (Minecraft) y comprueba qué rango de memoria fue alterado. Baneo permanente instantáneo sin necesidad de más pruebas.

---

## 2. Intentar Matar `ctfmon.exe` o `dwm.exe` para "Borrar el Rastro"
- **Técnica errónea**: Forzar el cierre de `ctfmon.exe` o reiniciar `dwm.exe` creyendo que al reiniciar el proceso se borra la memoria.
- **Por qué falla**:
  - Matar `dwm.exe` causa el parpadeo negro inmediato de la pantalla y el reinicio forzado del Desktop Window Manager, lo cual queda grabado con eventos de fallo de compositor en `Application.evtx` (Evento 1000).
  - Matar `ctfmon.exe` rompe la entrada de texto del usuario en Windows.
  - Además, las cadenas residuales persisten también en `explorer.exe` y `taskhostw.exe`, por lo que el revisor encuentra el patrón en los otros procesos del sistema.
