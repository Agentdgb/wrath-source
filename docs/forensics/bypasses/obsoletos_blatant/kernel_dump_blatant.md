# Bypass Obsoleto / Blatant: Textos Planos en Memoria y Bloqueo de Dumps

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Prácticas deficientes que exponen al usuario de forma fulminante ante un escaneo de Live Kernel Dump con `RedLotus`.

---

## 1. Cadenas de Texto Plano Hardcodeadas (Hardcoded Plaintext Strings)
- **Técnica errónea**: Compilar loaders o cheats sin protección de cadenas, dejando en el binario textos como:
  `"Wrath Loader"`, `"Aimbot"`, `"Killaura"`, `"ESP"`, `"Injecting into Minecraft..."`, `"discord.gg/..."`, `"github.com/..."`.
- **Por qué falla ante RedLotus (Baneo Inmediato)**:
  - Cuando el revisor crea el Live Dump en System Informer y ejecuta RedLotus:
    El analizador corre un algoritmo de escaneo de cadenas sobre los megabytes volcados.
  - Al encontrar palabras clave de cheats en la memoria de procesos o buffers del kernel, genera un archivo `.txt` con los extractos resaltados.
  - Esta evidencia es contundente: el analista lee directamente el nombre del cheat y sus funciones.

---

## 2. Inyección y Ejecución mediante LOLBins (`rundll32` / `regsvr32`)
- **Técnica errónea**: Ejecutar comandos como:
  ```cmd
  rundll32.exe cheat.dll,DllRegisterServer
  regsvr32.exe /s cheat.dll
  ```
- **Por qué falla**:
  - Las estructuras `PEB` y los buffers de argumentos de procesos en el kernel conservan las líneas de comando en texto plano.
  - RedLotus tiene reglas automatizadas para detectar el uso anómalo de `rundll32.exe` y `regsvr32.exe` cargando archivos fuera de `System32`.

---

## 3. Intentar Bloquear o Sabotear la Creación del Dump
- **Técnica errónea**: Utilizar trampas antianálisis que hookean funciones del kernel o cierran la sesión cuando detectan que `SystemInformer.exe` intenta invocar `NtSystemDebugControl` o APIs de captura de memoria.
- **Por qué falla**:
  - Si el revisor de screenshare intenta generar el dump y System Informer se congela, muestra un error de acceso denegado o el sistema colapsa con pantalla azul, el revisor marca inmediatamente **Bloqueo Activo de Herramientas Forenses (Auto-Ban)**.
