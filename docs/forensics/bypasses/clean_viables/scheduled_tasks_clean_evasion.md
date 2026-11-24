# Bypass Limpio: Cero Dependencia del Programador de Tareas

## Nivel: CLEAN / VIABLE (Arquitectura Wrath / VANTA)

Este bypass asegura la ausencia total de artefactos en el Programador de Tareas de Windows, tanto en memoria volátil (`Schedule` service) como en disco (`Tasks\`) y registro (`TaskCache`).

---

## 1. Vector Principal: Prohibición Absoluta de Tareas Programadas
*Regla de arquitectura de Wrath / VANTA:*

1. **Cero Creación de Tareas**:
   - Wrath / VANTA es un cliente de ejecución en memoria bajo demanda; **jamás crea, registra ni programa tareas en Windows**.
   - No se utiliza la API `ITaskService`, no se invoca `schtasks.exe`, ni se utiliza `at.exe`.
2. **Impacto Forense en las Inspecciones de Screenshare**:
   - **En Memoria del Servicio `Schedule`**:
     - Al volcar el servicio con System Informer o MagnetProcessCapture y buscar `<Command>` o `<Action>`, **no existe ninguna entrada perteneciente a Wrath**.
     - Cero invocaciones a `cmd.exe` o `powershell.exe` en los buffers del servicio.
   - **En Disco (`C:\Windows\System32\Tasks`)**:
     - Al ordenar por fecha de modificación en Everything, **ningún archivo de tarea fue creado o modificado en la fecha/hora actual**.
     - Cero transacciones de borrado de tareas en el diario USN registradas por `Journal-Tool`.
   - **En Registro (`TaskCache\Tasks` y `TaskCache\Tree`)**:
     - Cero claves huérfanas, cero tareas con caracteres Unicode y cero registros desasignados en slack space recuperables por Registry Explorer o `Task-Scheduler-Parser.ps1`.
   - **En Signature Checker**:
     - Al correr `bstrings` sobre `Tasks` y verificar firmas con BACA, el 100% de las tareas corresponden a binarios oficiales de Windows con firma válida de Microsoft.

---

## 2. Preservación Estricta de Tareas Nativas del Sistema
1. Wrath jamás modifica la configuración ni los triggers de tareas críticas del sistema operativo (como `SvcRestartTask` o tareas de telemetría de Windows).
2. Los triggers y marcas de tiempo de las tareas nativas conservan su configuración original de fábrica, impidiendo que el revisor detecte anomalías en las opciones de logon o inicio.

---

## 3. Cero COM Handler Hijacking
1. Wrath no manipula interfaces COM de tareas programadas (`Get-ScheduledTaskComHandler.ps1`).
2. No se registran Custom Handlers en `CLSID` del registro para ejecución indirecta.
