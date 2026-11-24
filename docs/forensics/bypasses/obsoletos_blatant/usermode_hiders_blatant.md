# Bypass Obsoleto / Blatant: Uso de Usermode Hiders y Strings Planas en el Juego

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Las dos fallas más habituales en la escena de FiveM y Minecraft que aseguran el baneo directo en escaneos de memoria con System Informer y HxD.

---

## 1. Usar Herramientas "File Hider" / "Folder Hider" Pre-Screenshare
- **Técnica errónea**:
  Ejecutar un programa de terceros (`hider.exe`, scripts `.bat` de ocultación) justo antes de que el revisor se conecte por AnyDesk para que oculte la carpeta del cheat de Windows Explorer.
- **Por qué falla ante los revisores (Baneo Inmediato)**:
  - Para ocultar carpetas en Windows Explorer, el hider interactúa con `explorer.exe` o `csrss.exe`.
  - El revisor hace un dump de `explorer.exe` y `csrss.exe`, abre el archivo en HxD y busca:
    `hide`, `hider`, `hidden` o nombres de herramientas públicas.
  - HxD expone de inmediato las cadenas del hider y las rutas de las carpetas que intentó ocultar.
  - **Doctrina del Revisor**: Un jugador legítimo jamás ejecuta un software de ocultamiento de archivos. La sola presencia de residuos de un hider en la memoria del sistema es prueba indiscutible de encubrimiento premeditado (*Intent to Deceive = Ban Permanente*).

---

## 2. Dejar Cadenas de Texto Plano en el Payload Inyectado
- **Técnica errónea**:
  Compilar la DLL del cheat con títulos de ventana, cadenas de configuración y mensajes de log en texto plano:
  `"Killaura Active"`, `"Reach Modifier"`, `"Aimbot Target"`, `"Custom Lua Executor"`, `"Cheat Menu v2"`.
- **Por qué falla ante el volcado del proceso del juego**:
  - El revisor hace clic derecho en el proceso del juego (`javaw.exe` / `FiveM`), genera el dump y lo abre en HxD.
  - Con Ctrl + F busca términos obvios: `menu`, `reach`, `aimbot`, `executor`, `dumper`.
  - HxD muestra las cadenas completas en la columna de texto.
  - El revisor lee directamente los nombres de las funciones del cheat dentro de la memoria del proceso del juego: el caso se cierra con baneo instantáneo.

---

## 3. Inyectar Hooks en `csrss.exe`
- **Técnica errónea**: Intentar hookear APIs de bajo nivel dentro de `csrss.exe` para ocultar procesos o ventanas.
- **Por qué falla**:
  - `csrss.exe` es un proceso crítico del subsistema de Windows protegido por el kernel.
  - Abrir un handle con permisos de escritura hacia `csrss.exe` desde un proceso no de sistema despierta alertas inmediatas en cualquier auditoría de handles (`windows.handles` en Volatility o System Informer) y suele provocar pantallazos azules por violaciones de acceso de memoria.
