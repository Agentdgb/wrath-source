# Detección Forense: Volcado de Memoria RAM y Extracción de Comandos CMD

## 1. Identificación y Alcance de la Técnica
- **Herramientas**:
  - `WinPmem64` (e.g. `winpmem_mini_x64_rc2.exe`): Adquisición de la memoria física completa (`ram.raw`).
  - `HxD Hex Editor`: Editor hexadecimal y escáner de patrones sobre el dump en bruto.
  - `Volatility 3`: Plugins automatizados `windows.cmdline` y `windows.consoles`.
- **Objetivo**: Recuperar comandos ejecutados previamente en el Símbolo del Sistema (`cmd.exe`), rutas de inyección, variables de entorno y salidas de programas desde los búferes residuales en la memoria RAM física, incluso si la ventana de CMD ya fue cerrada.

---

## 2. Fundamento Forense de la Persistencia en RAM
1. **Búferes de Consola (`conhost.exe` / `cmd.exe`)**:
   - Cada instancia de CMD crea un proceso de host de consola (`conhost.exe`) que mantiene en memoria RAM los búferes de entrada y salida (`CONSOLE_SCREEN_BUFFER_INFO`), el historial de comandos ejecutados y los títulos de ventana.
2. **Ciclo de Vida de las Páginas de Memoria**:
   - Al cerrar la ventana de CMD (`ExitProcess`), el sistema operativo marca las páginas de memoria como libres o en espera (*Standby / Free List*).
   - Sin embargo, Windows **no sobrescribe físicamente con ceros la memoria RAM liberada de forma inmediata**; los datos permanecen intactos en los chips de RAM hasta que otro proceso demande memoria y sobrescriba esos sectores.
3. **Localización mediante Patrones Hexadecimales**:
   - El analista busca la cadena del título de ventana en hexadecimal dentro de `ram.raw`:
     - Modo Administrador: `"Administrator: Command Prompt"`
       `41 64 6D 69 6E 69 73 74 72 61 74 6F 72 3A 20 43 6F 6D 6D 61 6E 64 20 50 72 6F 6D 70 74`
     - Modo Estándar: `"Command Prompt"`
       `43 6F 6D 6D 61 6E 64 20 50 72 6F 6D 70 74`
   - En la vecindad del offset hallado en HxD, aparecen los comandos exactos tipeados por el usuario, marcas de tiempo y rutas de carpetas utilizadas para compilar o inyectar software.

---

## 3. Firmas Buscadas en Screenshares
- Comandos ejecutados para iniciar loaders: `loader.exe`, `python inject.py`, scripts de compilación `cmake --build`.
- Rutas a carpetas de cheats: `cd C:\Users\...\Desktop\Cheat`.
- Comandos de limpieza destructiva tipeados manualmente: `wevtutil cl`, `del /f /s`, `net stop dps`.
