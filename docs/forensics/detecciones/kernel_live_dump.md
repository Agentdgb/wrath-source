# Detección Forense: Live Kernel Dump y RedLotus (Spokwn Analyzer)

## 1. Identificación y Alcance de la Técnica
- **Herramienta de Captura**: `System Informer (Process Hacker)` (Opción: *Create kernel memory dump > Live kernel dump*) o Administrador de Tareas (*Create dump file* sobre proceso System PID 4).
- **Herramienta de Análisis**: `RedLotus Kernel Live Dump Analyzer` (desarrollado por Spokwn / RedLotus).
- **Objetivo**: Extracción de un volcado en caliente (live dump `.dmp`) de la memoria del kernel (`ntoskrnl.exe`), estructuras de procesos (`EPROCESS`), pools de memoria (`NonPagedPool`, `PagedPool`) y páginas de usuario/hipervisor en la sesión activa del sistema.
- **Ventaja en Screenshares**:
  - Evita tener que reiniciar o esperar un BSOD.
  - No depende de que los archivos estén en disco: escanea directamente la memoria RAM volcada buscando cadenas de texto, trazas de inyección, comandos ejecutados y buffers residuales.

---

## 2. Opciones de Volcado en System Informer
| Opción | Significado Forense |
| :--- | :--- |
| **Use dump storage stack** | Almacena información detallada de pilas de E/S y operaciones de disco. |
| **Compress memory pages** | Reduce el tamaño del archivo `.dmp` para facilitar su análisis. |
| **Capture user pages** | Incluye las páginas de memoria de procesos de usuario (captura la memoria de `javaw.exe`, navegadores y loaders). |
| **Capture Hypervisor pages** | Vuelca páginas de capas de virtualización (Hyper-V / VBS). |
| **Only kernel thread stacks**| Genera un dump mínimo centrado únicamente en las pilas de ejecución de hilos del kernel. |

---

## 3. Firmas y Palabras Clave Auditadas por RedLotus
RedLotus escanea automáticamente el archivo `.dmp` buscando patrones sintomáticos de trampas y bypasses:

1. **Comandos de Scripting / PowerShell**:
   - `iex`, `iwr`, `Invoke-Expression`, `DownloadString`, `-EncodedCommand`, fragmentos de base64 y URLs de repositorios de cheats.
2. **Binarios Living-off-the-Land (LOLBins)**:
   - Invocaciones a `rundll32.exe`, `regsvr32.exe`, `forfiles.exe`, `wmic.exe` utilizadas para ejecutar DLLs o scripts de forma indirecta.
3. **Operaciones Destructivas de Registro**:
   - `reg delete`, comandos de vaciado de subclaves de BAM, UserAssist o AppCompatCache.
4. **Firmas de Procesos Modificados**:
   - Trazas de *Process Hollowing*, inyecciones de shellcode en procesos legítimos (`notepad.exe`, `svchost.exe`) o regiones de memoria ejecutables anómalas.
5. **Cadenas de Texto Plano de Cheats**:
   - Nombres de módulos (`Aimbot`, `Killaura`, `ESP`, `Bypass`, `Injection`), rutas a carpetas de compilación (`C:\Users\...\Desktop\Cheat`) y nombres de proyectos.
