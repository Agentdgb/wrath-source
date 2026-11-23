# Detección Forense: Usermode Hiders e Inyección en Memoria del Juego

## 1. Identificación y Alcance de las Metodologías
Este análisis cubre dos técnicas directas aplicadas comúnmente en screenshares de FiveM y Minecraft:
1. **Detección de Inyección Directa en el Proceso del Juego**: Volcado de memoria del proceso anfitrión (`javaw.exe` / `FiveM_GTAProcess.exe`) y escaneo de cadenas en texto plano con HxD.
2. **Detección de Usermode Hiders en Procesos del Sistema**: Búsqueda de residuos de herramientas de ocultamiento de archivos en los volcados de memoria de `explorer.exe` y `csrss.exe`.

---

## 2. Metodología de Detección

### A. Método 1: Escaneo de Cadenas en el Proceso del Juego
1. **Adquisición**:
   - Con el juego abierto, el revisor hace clic derecho en el proceso en System Informer y selecciona `Create dump file` para generar el volcado `.dmp`.
2. **Búsqueda en HxD (Text String)**:
   - Se abre el dump en HxD y se buscan palabras clave que no pertenecen a una instalación limpia:
     - Nombres de menús y GUI: `menu`, `clickgui`, `hud`, `visuals`, `executor`.
     - Funciones de trampa: `aimbot`, `reach`, `killaura`, `velocity`, `dumper`.
     - Referencias a scripts o inyectores: `injector`, `payload`, nombres de proyectos de GitHub.
3. **Criterio de Baneo**:
   - Muchos cheats en userland dejan títulos de ventanas, mensajes de consola y nombres de módulos en texto plano en la sección `.rdata` o en el heap.
   - Encontrar estas cadenas dentro de la memoria del juego es **prueba directa irrefutable de inyección activa**.

### B. Método 2: Detección de Usermode Hiders en `explorer.exe` y `csrss.exe`
1. **Qué es un Usermode Hider**:
   - Una herramienta que el jugador ejecuta antes del screenshare para que Windows Explorer "oculte" la carpeta o archivo del cheat.
   - Para lograrlo, el hider debe hookear APIs como `NtQueryDirectoryFile` en `explorer.exe` o ejecutar un proceso en segundo plano que filtre los listados del sistema de archivos.
2. **El Fallo Fatal del Hider**:
   - El hider oculta los archivos del cheat en el disco, pero **no oculta su propia ejecución en la memoria del sistema**.
   - Al interactuar con Windows Explorer, las cadenas del ejecutable del hider, los comandos de ocultamiento (`hide`, `hider`, `hidden`) y las rutas de las carpetas filtradas quedan grabadas en la memoria de `explorer.exe` y `csrss.exe`.
3. **Criterio de Baneo**:
   - El revisor hace un dump de `explorer.exe` y `csrss.exe` y busca `hide` en HxD.
   - Encontrar referencias a una herramienta de ocultamiento de archivos es causal directa de baneo automático por **Intento de Encubrimiento / Dolo Forense (Intent to Deceive)**.
