# Bypass Obsoleto / Blatant: Cheats y Loaders Desarrollados en C# / .NET

## Nivel: RECHAZADO / OBSOLETO / BLATANT

El desarrollo de cargadores, clickers e inyectores en C# (.NET / WPF / WinForms) genera una huella forense permanente e imborrable en el sistema operativo.

---

## 1. Crear Loaders o Inyectores en C# / .NET
- **Técnica errónea**:
  Desarrollar la interfaz del cheat o el inyector en C# usando Visual Studio (WPF o Windows Forms) debido a la facilidad de diseño de interfaces gráficas.
- **Por qué falla ante las revisiones forenses (Baneo Inmediato)**:
  1. **Registros de Crash en `Application.evtx` (Evento 1026)**:
     - Si el loader en C# tiene una excepción no capturada (por ejemplo, al intentar inyectar en Minecraft sin privilegios o cuando el proceso se cierra inesperadamente), el CLR registra automáticamente el **Event ID 1026 con Source `.NET Runtime`**.
     - El log guarda en texto plano el nombre del proyecto (`MyCheatLoader.exe`), la versión y la ruta completa del archivo en el momento del fallo.
  2. **Archivos Residuales en `UsageLogs`**:
     - El runtime de .NET crea automáticamente un archivo de registro en:
       `%LOCALAPPDATA%\Microsoft\CLR_v4.0\UsageLogs\CheatLoader.exe.log`
     - Aunque el usuario borre el ejecutable del escritorio, este log de uso permanece en AppData registrando todas las dependencias del framework que utilizó el programa.
  3. **Identificación Inmediata en Prefetch**:
     - Al analizar el archivo `.pf` en `C:\Windows\Prefetch\`, la presencia de `mscoree.dll` y `clr.dll` en la lista de dependencias demuestra sin lugar a dudas que se trató de una aplicación gestionada de .NET.

---

## 2. Borrar el Archivo `.exe` de C# Creyendo que No Quedan Trazas
- **Técnica errónea**:
  Descargar el inyector de C#, ejecutarlo, inyectar el DLL y presionar `Shift + Supr` sobre el ejecutable.
- **Por qué falla**:
  - Los revisores cruzan las fechas de Prefetch, AmCache y los logs de `.NET Runtime` en el Visor de Eventos.
  - La presencia simultánea del hash SHA-1 en AmCache y los registros del runtime en `Application.evtx` demuestra de forma irrefutable que el usuario ejecutó una herramienta de trampas desarrollada en .NET.
