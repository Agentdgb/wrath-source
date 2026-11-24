# Detección Forense: MagnetProcessCapture + bstrings + Base64Hunter

## 1. Identificación y Alcance del Flujo de Detección
- **Herramientas del Flujo**:
  1. `Magnet Process Capture` (Magnet Forensics): Herramienta gráfica/CLI para capturar un volcado selectivo de la memoria virtual completa de un proceso activo (típicamente `javaw.exe`).
  2. `bstrings.exe` (Eric Zimmerman Tools): Extractor avanzado de cadenas ASCII y Unicode (UTF-16) con soporte de expresiones regulares para procesar el archivo `.dmp`.
  3. `Base64Hunter` (desarrollado por Nacio78923): Herramienta especializada para cazar, extraer y decodificar masivamente cadenas codificadas en Base64 presentes en volcados de memoria y archivos binarios.
- **Objetivo**: Extraer cadenas de configuración, nombres de módulos de trampas, URLs de paneles/webhooks y scripts que permanezcan en la memoria RAM del proceso del juego.

---

## 2. Flujo de Inspección en Screenshares
El revisor ejecuta la secuencia automatizada:

1. **Captura de Memoria**:
   - Con Minecraft abierto, el revisor selecciona el proceso `javaw.exe` en Magnet Process Capture y genera un archivo de volcado completo `javaw.dmp`.
2. **Extracción Exhaustiva de Cadenas con `bstrings`**:
   ```cmd
   bstrings.exe -f javaw.dmp -o strings_output.txt --ls --fs
   ```
   - Extrae todas las cadenas de texto plano (ASCII y Unicode) de longitud mayor a 4 caracteres.
   - Aplica filtros regex para detectar URLs (`http://`, `https://`), rutas de archivos (`C:\...`) y argumentos.
3. **Caza de Payloads con `Base64Hunter`**:
   - Escanea el archivo `.dmp` o la salida de `bstrings` buscando patrones de texto compatibles con el alfabeto de Base64 (`[A-Za-z0-9+/=]{8,}`).
   - **Decodificación Automática**: Decodifica en caliente cada bloque de base64 y analiza el texto resultante contra palabras clave de trampas (`aimbot`, `esp`, `killaura`, `webhook`, `inject`, `bypass`).

---

## 3. Firmas Buscadas y Reglas de Flag
- **Flag 1 — Coincidencias en Base64Hunter**: Cadenas en base64 que al decodificarse revelen URLs de Discord Webhook, scripts de PowerShell (`powershell -enc`), o parámetros de módulos de cheats.
- **Flag 2 — Cadenas de Texto Plano de Módulos**: Búsqueda en `strings_output.txt` de términos como `Aimbot`, `Hitboxes`, `AutoClicker`, `Reach`, `Velocity`, `NoFall`, `StringScrub`.
- **Flag 3 — Nombres de Archivos de Clases Java Modificadas**: Presencia de nombres de paquetes o clases no estándar dentro de la memoria de `javaw.exe` que no pertenezcan al juego vanilla ni a mods autorizados.
