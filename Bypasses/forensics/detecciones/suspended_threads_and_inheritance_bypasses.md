# Detección Forense: Suspensión de Hilos en Servicios y Alteración de Herencia en Registro

## 1. Identificación y Alcance de las Técnicas
- **Técnicas Auditadas**:
  1. **Thread-Level Service Tampering**: Suspensión selectiva de hilos de trabajo en servicios críticos de Windows para evitar la generación de telemetría sin cambiar el estado del servicio a `Stopped`.
  2. **Disabled Registry Inheritance**: Alteración de permisos (DACL) y remoción de herencia en claves del registro para impedir que el sistema operativo registre la actividad del usuario.
- **Servicios Blanco Típicos**:
  - `SysMain` (`sechost.dll`): Generación de Prefetch.
  - `PcaSvc` (`pcasvc.dll`): Program Compatibility Assistant (ejecución de binarios).
  - `DPS` (`dps.dll`): Diagnostic Policy Service (telemetría SRUM).
  - `EventLog` (`wevtsvc.dll`): Generación de registros de eventos de Windows.
  - `DusmSvc` (`wlanapi.dll`): Registro de uso de red por proceso.
  - `CDPUserSvc` (`ucrtbase.dll`): Sincronización de Windows Timeline (`ActivitiesCache.db`).

---

## 2. Metodología de Detección en Screenshares

### A. Inspección Directa de Hilos en System Informer
1. **Auditoría de Procesos `svchost.exe`**:
   - El revisor abre System Informer, localiza el proceso `svchost.exe` que hospeda el servicio sospechoso (e.g. `PcaSvc` o `SysMain`).
   - Abre las propiedades del proceso y se dirige a la pestaña **Threads**.
   - Ordena los hilos por la columna **State** (Estado).
2. **Bandera Crítica**:
   - En un sistema Windows funcional normal, los hilos de los servicios se encuentran en estado `Wait:UserRequest`, `Wait:Executive` o `Running`.
   - Si se encuentra un hilo cuyo punto de entrada (*Start Address*) reside en `pcasvc.dll`, `dps.dll` o `sechost.dll` en estado **`Suspended`**, se declara **Bypass por Manipulación de Hilos (Ban Inmediato)**.

### B. Detección por Asincronía Forense (Generation Gap)
1. **La Prueba Dinámica en Vivo**:
   - Los revisores ejecutan una prueba funcional durante el screenshare: lanzan una aplicación legítima e inofensiva de Windows (e.g. `calc.exe` o `notepad.exe`).
   - Comprueban si Windows genera el archivo `CALC.EXE-XXXXXXXX.pf` en `C:\Windows\Prefetch\` o si añade una nueva fila en la clave `BAM`.
2. **El Fallo del Bypass**:
   - Si el servicio `SysMain` figura como `Running` en `get-service`, pero **no se genera ningún prefetch** para `calc.exe`, se confirma que el servicio está saboteado internamente en memoria.

### C. Auditoría de Herencia y Permisos en el Registro (DACL Inspection)
- Herramientas automáticas (Echo, Paladin) auditan las listas de control de acceso discrecional (DACL) de claves como:
  - `HKLM\SYSTEM\CurrentControlSet\Services\bam\State\UserSettings\<SID>`
  - `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\RecentDocs`
  - `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\UserAssist`
- **Banderas de Alerta**:
  - Herencia deshabilitada (`SE_DACL_AUTO_INHERITED = False`).
  - Permisos denegados (`ACCESS_DENIED`) para la cuenta `SYSTEM` o `Administrators`.
  - Ausencia absoluta de nuevos registros en un usuario que lleva horas jugando.
