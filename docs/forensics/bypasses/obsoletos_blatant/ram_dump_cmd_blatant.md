# Bypass Obsoleto / Blatant: Ejecución desde CMD y el Mito de `cls`

## Nivel: RECHAZADO / OBSOLETO / BLATANT

Errores fundamentales de concepto sobre la memoria RAM que exponen por completo al usuario ante herramientas de volcado físico como WinPmem y HxD.

---

## 1. Ejecutar el Loader desde una Ventana de CMD y Cerrarla
- **Técnica errónea**: Abrir una terminal de CMD, navegar a la carpeta de trampas, tipear `loader.exe` y luego cerrar la ventana de CMD creyendo que el rastro desapareció al cerrarse el proceso.
- **Por qué falla ante WinPmem y HxD (Baneo Inmediato)**:
  - Windows no borra físicamente la RAM al terminar un proceso.
  - Al ejecutar `WinPmem64.exe ram.raw` y abrir el volcado en HxD:
    El analista busca el string en hexadecimal del título de ventana (`Administrator: Command Prompt`) y encuentra de inmediato:
    `C:\Users\Admin\Desktop\Cheat\loader.exe -target javaw.exe`
  - La prueba es irrefutable: muestra la ruta exacta, los argumentos de lanzamiento y la salida en pantalla generada por el programa.

---

## 2. El Mito del Comando `cls` (Limpiar Pantalla)
- **Técnica errónea**: Tipear `cls` en CMD antes de cerrar la ventana, creyendo que esto "borra" los comandos ejecutados de la memoria.
- **Por qué falla**:
  - `cls` es una función cosmética que únicamente reposiciona el cursor en la esquina superior izquierda del búfer y llena de espacios en blanco el área visible actual de la ventana.
  - **No vacía el historial de comandos en memoria**: Los comandos previamente ejecutados se almacenan en un ring buffer de historial dentro del proceso `conhost.exe`.
  - En el dump de RAM, la totalidad del historial sigue estando presente y legible.

---

## 3. Intentar Bloquear la Adquisición de Memoria de WinPmem
- **Técnica errónea**: Intentar descargar o bloquear el driver `winpmem.sys` en tiempo de ejecución.
- **Por qué falla**:
  - WinPmem utiliza un controlador firmado legítimo con permisos de Administrador para abrir `\Device\PhysicalMemory`.
  - Provocar un fallo o denegar el acceso a la herramienta durante el screenshare es catalogado como **Obstrucción Activa del Proceso Forense (Auto-Ban)**.
