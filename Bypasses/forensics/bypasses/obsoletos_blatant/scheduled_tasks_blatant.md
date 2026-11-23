# Bypass Obsoleto / Blatant: Uso de Tareas Programadas para Inyección

## Nivel: RECHAZADO / OBSOLETO / BLATANT

El uso del Programador de Tareas para ejecutar o reiniciar trampas es una de las técnicas más fáciles de detectar y con mayor tasa de baneo automático en screenshares forenses.

---

## 1. Crear Tareas Programadas para Ejecutar Cheats o Scripts
- **Técnica errónea**: Crear una tarea con `schtasks /create` para ejecutar `cmd.exe /c start cheat.exe` o scripts de limpieza al iniciar sesión o desbloquear el equipo.
- **Por qué falla (Baneo Inmediato)**:
  1. **Detección en Memoria de `Schedule`**:
     - Al volcar el servicio en System Informer y buscar `<Command>`, aparece:
       `<Command>C:\Windows\System32\cmd.exe</Command>`
       `<Arguments>/c start cheat.exe</Arguments>`
     - En las normativas de screenshare de la comunidad (Tome II, Red Lotus, Echo, Paladin), **una tarea programada que invoque CMD es causal directa de baneo permanente por "Scheduled CMD Bypass Attempt"**.
  2. **BACA's Signature Checker**:
     - Al parsear las rutas de `C:\Windows\System32\Tasks` con `bstrings`, la herramienta detecta que la tarea apunta a un ejecutable no firmado o ubicado en carpetas de usuario.

---

## 2. Borrar la Tarea Inmediatamente Antes del Screenshare (`schtasks /delete`)
- **Técnica errónea**:
  Crear la tarea, ejecutar el cheat y borrar la tarea con `schtasks /delete /tn "MyTask" /f` cuando el staff pide screenshare.
- **Por qué falla**:
  - **Journal-Tool (USN Journal)**: El diario NTFS registra la transacción de borrado del archivo XML en `C:\Windows\System32\Tasks` con el timestamp de hace 5 minutos.
  - **Memoria Volátil**: La memoria del servicio `Schedule` conserva en sus buffers el comando y la acción durante al menos 15 a 30 minutos después de que el archivo en disco fue eliminado.
  - **Registry Explorer**: La colmena `SOFTWARE` en `TaskCache\Tasks` conserva el GUID y los parámetros de la tarea en el slack space de las celdas borradas.
  - El revisor obtiene el nombre del archivo borrado desde el diario USN y el comando ejecutado desde la memoria de System Informer: la prueba de encubrimiento es 100% concluyente.

---

## 3. Manipular Tareas Nativas de Windows (`SvcRestartTask`)
- **Técnica errónea**: Modificar los desencadenadores (triggers) de `SvcRestartTask` para que se ejecute al inicio de sesión o al loguearse el usuario.
- **Por qué falla**:
  - `SvcRestartTask` en un sistema Windows limpio nunca tiene triggers configurados en startup o user logon.
  - Al revisar los triggers en Task Scheduler o Search Everything, la fecha de modificación reciente y el trigger alterado delatan inmediatamente manipulación manual del subsistema de servicios.

---

## 4. Ocultamiento mediante Caracteres Unicode Invisibles
- **Técnica errónea**: Nombrar la tarea con caracteres Unicode no imprimibles para que el GUI oficial de Windows (`taskschd.msc`) falle o no la muestre en la lista.
- **Por qué falla**:
  - Los revisores experimentados no dependen del GUI oficial.
  - Utilizan `Task-Scheduler-Parser.ps1`, `bstrings` directamente sobre el disco y volcados de memoria, los cuales leen los flujos binarios puros ignorando los errores del GUI y marcando la presencia de caracteres Unicode como intento de evasión deliberada.
