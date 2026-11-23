# Detección Forense: Comandos Interactivos en Vivo (CMD y PowerShell en Screenshares)

## 1. Identificación y Alcance del Procedimiento
- **Vector**: Ejecución interactiva de comandos nativos de CMD y PowerShell por parte del revisor o staff durante una llamada de screenshare (AnyDesk, Discord, TeamViewer).
- **Objetivo**: Verificación rápida sin herramientas complejas para detectar servicios deshabilitados, procesos anómalos en ejecución, argumentos de línea de comandos, archivos modificados recientemente e integridad del disco.

---

## 2. Comandos Clave Utilizados en Revisiones Manuales

### A. Auditoría de Servicios Obligatorios
- **Comandos**:
  ```powershell
  get-service | findstr -i "pcasvc"; get-service | findstr -i "DPS"; get-service | findstr -i "Diagtrack"; get-service | findstr -i "sysmain"; get-service | findstr -i "eventlog"
  ```
  o en CMD:
  ```cmd
  sc query pcasvc
  sc query DPS
  sc query SysMain
  ```
- **Qué buscan**:
  - Si alguno de estos servicios clave figura como `Stopped`, `Disabled` o no aparece.
  - La presencia de servicios detenidos indica inmediatamente que se utilizó un bypass público antiguo para evitar registros de telemetría.

### B. Inspección de Procesos y Líneas de Comando
- **Comandos**:
  ```powershell
  Get-WmiObject Win32_Process | Select-Object Name, ProcessId, CommandLine
  Get-Process | Where-Object { $_.CPU -gt 50 }
  tasklist /svc
  ```
- **Qué buscan**:
  - Procesos en segundo plano no estándar (`loader.exe`, scripts de Python/Node, ejecutables compilados desde rutas temporales).
  - Rutas y parámetros en `CommandLine` (e.g. ejecutables lanzados con `-inject`, flags ocultos o scripts `.bat`/`.ps1`).

### C. Búsqueda de Archivos Modificados en las Últimas 24 Horas
- **Comando**:
  ```cmd
  forfiles /P C:\ /S /D -1 /C "cmd /c if @isdir==FALSE echo @path @fdate @ftime"
  ```
- **Qué busca**:
  - Recorre recursivamente todo el disco `C:\` identificando archivos creados o editados en las últimas 24 horas (`/D -1`).
  - Cualquier archivo de cheat, configuración, DLL o inyector depositado en el disco durante el día salta en la lista con su ruta exacta y hora de modificación.

### D. Integridad del Sistema de Archivos y Dirty Bit
- **Comando**:
  ```cmd
  fsutil dirty query C:
  fsutil fsinfo ntfsinfo C:
  ```
- **Qué busca**:
  - Comprueba si el volumen `C:` tiene activo el flag "dirty" (volumen sucio/corrupto).
  - Un dirty bit activo delata apagados forzados, desconexiones intencionales o manipulación de bajo nivel en sectores NTFS para eludir revisiones.

### E. Programas en Inicio (Startup) y Logs Recientes
- **Comandos**:
  ```powershell
  Get-CimInstance Win32_StartupCommand | Select-Object Name, Command, Location
  Get-EventLog -LogName System -Newest 10
  ```
- **Qué buscan**:
  - Persistencia de herramientas en el inicio de Windows o errores recientes en el sistema (como caídas de servicios o eventos 7036).
