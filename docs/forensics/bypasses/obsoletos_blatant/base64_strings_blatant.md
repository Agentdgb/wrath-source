# Bypass Obsoleto / Blatant: Uso de Base64 y Cadenas Planas en Memoria

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Prácticas ingenuas y quemadas que garantizan la detección inmediata al ser analizadas con Magnet Process Capture, `bstrings` y `Base64Hunter`.

---

## 1. Usar Base64 para Ocultar Cadenas, URLs o Webhooks
- **Técnica errónea**: Codificar la URL del webhook de Discord o los nombres de funciones en Base64 dentro del código:
  `"aHR0cHM6Ly9kaXNjb3JkLmNvbS9hcGkvd2ViaG9va3Mv..."`
  o ejecutar comandos con `-EncodedCommand <Base64>`.
- **Por qué falla ante Base64Hunter (Baneo Inmediato)**:
  - Base64Hunter escanea todo el archivo `.dmp` buscando patrones que coincidan con el alfabeto de Base64 (`[A-Za-z0-9+/=]`).
  - La herramienta decodifica automáticamente cada bloque y busca palabras clave como `discord`, `webhook`, `cheat`, `inject`.
  - El revisor obtiene en texto plano la URL y los comandos decodificados en el reporte final.
  - Usar Base64 es el equivalente forense a escribir la trampa en texto plano.

---

## 2. Dejar Cadenas de Texto Plano en el DLL o Memoria
- **Técnica errónea**: Dejar nombres literales en el código como:
  `"Aimbot"`, `"Killaura"`, `"Reach Distance"`, `"ESP Boxes"`, `"Fly Module"`.
- **Por qué falla ante `bstrings`**:
  - Al ejecutar:
    ```cmd
    bstrings.exe -f javaw.dmp -o strings.txt
    ```
    La herramienta vuelca todas las cadenas de texto del proceso.
  - El revisor aplica filtros con `findstr`:
    ```cmd
    findstr /i "killaura aimbot reach velocity" strings.txt
    ```
  - La terminal devuelve las líneas exactas con los nombres de los módulos del cheat dentro de la memoria de `javaw.exe`.

---

## 3. Intentar Bloquear o Corromper `MiniDumpWriteDump`
- **Técnica errónea**: Interceptar en el proceso de Minecraft las APIs `MiniDumpWriteDump` de `dbgcore.dll` / `dbghelp.dll` para que Magnet Process Capture falle con error de acceso denegado o genere un dump de 0 bytes.
- **Por qué falla**:
  - Magnet Process Capture es una herramienta oficial utilizada rutinariamente por los analistas forenses.
  - Si la herramienta falla al intentar volcar el proceso `javaw.exe` mientras funciona perfectamente en otros procesos, el analista marca al usuario con **`Memory Dump Blocked / Tampering`**, lo cual resulta en baneo inmediato por obstrucción del screenshare.
