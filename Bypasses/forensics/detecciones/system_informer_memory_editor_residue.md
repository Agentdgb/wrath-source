# Detección Forense: Residuos de Edición Manual de Memoria en Procesos del Sistema (System Informer / Process Hacker)

## 1. Identificación y Alcance de la Técnica
- **Herramienta Auditada**: `System Informer` / `Process Hacker` (editor de memoria interactivo en GUI).
- **Procesos Objetivo del Sistema**:
  - `ctfmon.exe` (CTF Loader / Text Input & Accessibility Subsystem)
  - `explorer.exe` (Windows Shell)
  - `taskhostw.exe` (Host Process for Windows Tasks)
  - `dwm.exe` (Desktop Window Manager)
- **Objetivo**: Demostrar que el usuario abrió manualmente el visor/editor de memoria de Process Hacker o System Informer para alterar bytes de memoria en un proceso activo (e.g., patchear funciones, congelar valores o inyectar shellcode de trampas).

---

## 2. Mecanismo Forense del Residuo en Memoria
1. **Interacción con el Subsistema de Ventanas y Accesibilidad**:
   - Cuando un usuario abre la ventana de inspección de memoria en Process Hacker y navega por los bloques virtuales de un proceso, la interfaz gráfica genera cadenas descriptivas en los títulos de ventana y controles de accesibilidad.
   - Estos textos interactúan con `ctfmon.exe` (gestión de entrada de texto), `dwm.exe` (composición gráfica de ventanas) y `explorer.exe`.
2. **Patrón de Firma Específico**:
   - En el volcado de memoria (`.dmp`) de cualquiera de estos cuatro procesos, se busca la cadena de formato generada exclusivamente por la ventana del editor de memoria de System Informer:
     ```
     ) (0x
     ```
3. **Estructura del Resultado Hallado**:
   ```
   (2804) (0x5a6beac000 - 0x5ac6beb0000)
   ```
   - **`(2804)`**: El Process ID (PID) del proceso que fue alterado o inspeccionado en memoria.
   - **`0x5a6beac000`**: Dirección virtual de inicio de la región de memoria inspeccionada o modificada.
   - **`0x5ac6beb0000`**: Dirección virtual de fin del bloque de memoria.

---

## 3. Criterio de Baneo en Screenshares
- Este formato de cadena es exclusivo de la interfaz del editor de memoria de Process Hacker. No existe ninguna aplicación legítima de Windows que genere este patrón exacto `(PID) (0xStart - 0xEnd)` en la memoria de `explorer.exe`, `ctfmon.exe` o `dwm.exe`.
- Su hallazgo en un screenshare es **prueba concluyente irrefutable de manipulación manual de memoria en caliente**, justificando la expulsión inmediata del usuario.
