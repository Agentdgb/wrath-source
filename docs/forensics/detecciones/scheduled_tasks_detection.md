# Detección Forense: Análisis Profundo de Tareas Programadas (Scheduled Tasks)

## 1. Identificación y Alcance de la Metodología
- **Vector**: Detección de persistencia, inyecciones indirectas y scripts de evasión configurados a través del Programador de Tareas de Windows (Task Scheduler / `schtasks.exe` / servicio `Schedule`).
- **Objetivo en Screenshares**: Identificar tareas programadas creadas por usuarios o cheats para ejecutar comandos (`cmd.exe`, `powershell.exe`), tareas borradas antes de la llamada, o tareas modificadas para reinicio forzado de servicios (e.g., `SvcRestartTask`).
- **Herramientas Utilizadas**:
  - `System Informer (Process Hacker)`: Volcado e inspección de memoria del servicio `Schedule` (`svchost.exe`).
  - `bstrings.exe`: Extracción de tags XML `<Command>` y `<Action>`.
  - `Search Everything`: Monitoreo en tiempo real de archivos modificados en `C:\Windows\System32\Tasks`.
  - `Journal-Tool`: Detección de tareas eliminadas mediante análisis de transacciones USN.
  - `Registry Explorer`: Análisis de `TaskCache\Tree` y `TaskCache\Tasks` (slack space y nombres Unicode).
  - `BACA's Signature Checker`: Verificación de firmas de las rutas extraídas de las tareas.
  - `Task-Scheduler-Parser.ps1` (ObsessiveBf): Parseo directo por script de la carpeta de tareas.

---

## 2. Flujo de Detección en Screenshares (Tome II / Red Lotus Methodology)

### A. Inspección en Memoria Volátil del Servicio `Schedule`
1. **Volcado Inmediato (< 15 minutos)**:
   - La memoria del servicio `Schedule` es altamente volátil; los buffers se reciclan tras 15-30 minutos.
   - En System Informer, el revisor filtra el servicio `Schedule` (o vuelca el proceso `svchost.exe` correspondiente / `mmc.exe` con Task Scheduler abierto).
2. **Filtrado por Tags XML**:
   - Se buscan las cadenas `<Command` y `<Action`:
     - `<Command>`: Revela el ejecutable exacto que la tarea lanza (e.g. `cmd.exe`, `powershell.exe`, `loader.exe`).
     - `<Action>` o `<Arguments>`: Revela los parámetros asociados.
   - **Regla Directa de Baneo**: Si se encuentra una tarea que ejecuta `cmd.exe` o scripts desde rutas no del sistema, se emite baneo automático por **Bypass Attempt**.

### B. Análisis de Disco y Tareas Eliminadas (Journal-Tool + Everything)
1. **Detección de Tareas Modificadas**:
   - En Search Everything, el analista filtra: `C:\Windows\System32\Tasks\*` ordenado por fecha de modificación (`Date Modified`).
   - Cualquier archivo XML modificado en la instancia actual del sistema o minutos antes del screenshare se audita a fondo.
2. **Tareas Borradas Pre-Screenshare (Journal-Tool)**:
   - Journal-Tool escanea las transacciones del diario NTFS buscando eventos `USN_REASON_FILE_DELETE` en la ruta `C:\Windows\System32\Tasks`.
   - Si el usuario borró la tarea antes de aceptar AnyDesk:
     - El nombre del archivo XML borrado aparece en Journal-Tool.
     - El comando ejecutado se recupera cruzándolo con las cadenas `<Command>` volcadas de la memoria del servicio `Schedule`.

### C. Análisis con bstrings y BACA's Signature Checker
1. Extracción de rutas en disco:
   ```cmd
   bstrings.exe -d C:\Windows\System32\Tasks --ls :\ -o .\tasks.txt
   bstrings.exe -d C:\Windows\System32\Tasks --ls "Command" -o .\commands.txt
   ```
2. Se limpian los delimitadores `<Command>` para aislar las rutas de archivos.
3. Se pasan todas las rutas por BACA's Signature Checker:
   - Cualquier binario programado que carezca de firma digital válida de Microsoft es marcado como ejecutable no confiable / trampa.

### D. Evasión mediante Nombres Unicode en Registro (`TaskCache`)
- Algunos cheats crean tareas con caracteres Unicode invisibles o especiales para crashear el GUI de `taskschd.msc`.
- Registry Explorer carga `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache` y extrae los nombres Unicode y los GUIDs de tareas borradas desde las celdas desasignadas (slack space).
