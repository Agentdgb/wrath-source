# Detección Forense: DLL Hijacking y Análisis de Módulos

## 1. Identificación y Alcance de la Técnica
- **Técnica**: DLL Hijacking / DLL Proxying / Search Order Hijacking.
- **Mecanismo**: Manipulación del orden de búsqueda de bibliotecas dinámicas de Windows para forzar que un proceso legítimo y firmado (e.g. `javaw.exe`, lanzadores de Minecraft, clientes de terceros) cargue una DLL no autorizada en lugar de la biblioteca original del sistema o del juego.
- **Tipos comunes**:
  - *Search Order Hijacking*: Colocar una DLL clonada (e.g., `version.dll`, `dxgi.dll`, `opengl32.dll`) en el mismo directorio del ejecutable de destino, el cual precede a `System32` en el orden de búsqueda.
  - *DLL Proxying*: Crear una DLL intermedia que reenvía todos los exports a la DLL legítima pero ejecuta código de inyección en su `DllMain`.
  - *Phantom DLL*: Colocar una DLL con un nombre que el proceso busca pero que no existe por defecto en Windows.

---

## 2. Herramientas Forenses y Métodos de Detección
Los analistas y escáneres automatizados (Echo, Paladin, Ocean, Avenge) detectan DLL Hijacking mediante:

1. **System Informer (Canary Version) / Process Explorer**:
   - **Auditoría de Firmas y Rutas**: Muestra la lista de todos los módulos cargados en el proceso (`PEB.Ldr`). Cualquier módulo sin firma digital válida de Microsoft, Oracle o Mojang, o que se cargue desde una ruta inusual (e.g. `version.dll` cargado desde `.minecraft/runtime/bin` en vez de `C:\Windows\System32`), se resalta automáticamente en color púrpura/rojo.
   - **Dump de csrss.exe / Handles**: Inspecciona handles a secciones de mapeo de archivos que no pertenecen a los directorios del sistema operativo.
2. **Process Monitor (ProcMon)**:
   - Filtros por `Operation = CreateFile` o `Load Image` buscando resultados `NAME NOT FOUND` en `System32` seguidos de `SUCCESS` en rutas de usuario, o eventos de carga de binarios desde carpetas con permisos de escritura para el usuario.
3. **Escáneres Especializados (DLLSpy, HijackLibs)**:
   - Verifican listas de DLLs históricamente vulnerables utilizadas en software de trampas (`d3d9.dll`, `d3d11.dll`, `version.dll`, `winhttp.dll`).
4. **Verificación Estática de la Carpeta del Juego**:
   - Los escáneres de screenshare recorren la carpeta de Java y de Minecraft antes de abrir el juego: cualquier `.dll` presente en la carpeta del ejecutable con hash desconocido o sin firma de confianza produce un **Ban Inmediato por Fake DLL**.
