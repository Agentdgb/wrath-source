# Errores Flagrantes y Maniobras Quemadas ante un Screenshare

## 1. Visión General de Técnicas Quemadas
Muchos usuarios y desarrolladores sin formación forense recurren a maniobras de pánico cuando son llamados a una revisión por screenshare. Estas acciones no solo fallan en ocultar la actividad, sino que generan **pruebas directas y concluyentes de manipulación deliberada** (*tampering*), resultando en sanciones automáticas inmediatas sin necesidad de encontrar el software original.

---

## 2. Maniobras de Pánico Flagrantes

### 1. El "Reinicio de Emergencia" (*Panic Reboot*)
* **Comportamiento**: Fingir un cuelgue de Windows, un pantallazo azul o forzar el botón de apagado de la máquina inmediatamente después de recibir la citación de screenshare.
* **Por Qué es Flagrante**:
  * En el visor de eventos (`System.evtx`), se genera de inmediato el **Event ID 6008** (*The previous system shutdown was unexpected*) o **Event ID 41** (*Kernel-Power*).
  * Los checkers experimentados cruzan el `EventRecordID` y la marca de tiempo de dicho evento con la hora exacta en la que se envió la notificación en Discord o en el servidor.
  * La correlación temporal entre la llamada y el reinicio abrupto se cataloga en todas las normativas competitivas como **Rechazo / Evasión de Revisión** (*Refusal to Screenshare*).

### 2. Ejecución de "Cleaners" o Scripts `.bat` / `.ps1`
* **Comportamiento**: Mantener un script de limpieza en el escritorio o en una memoria USB llamado `cleaner.bat`, `flush.bat` o `panic.exe` y ejecutarlo con doble clic.
* **Huellas Forenses Generadas**:
  * **Consola PowerShell / CMD**: Quedan registrados los comandos en `ConsoleHost_history.txt` si el usuario no controla la sesión.
  * **USN Journal**: Genera una ráfaga masiva de registros `USN_REASON_FILE_DELETE` en un intervalo de menos de 2 segundos.
  * **$LogFile**: Las transacciones de desasignación de clústeres saturan el búfer circular de NTFS.
  * **Prefetch / BAM**: Aparecen `cmd.exe`, `powershell.exe` o `cleaner.exe` ejecutados con marca de tiempo contemporánea a la congelación de la pantalla.

### 3. El Vaciado Total de Prefetch y Eventos
* **Comportamiento**: Ejecutar `del /q C:\Windows\Prefetch\*` y `wevtutil cl` para dejar el ordenador "sin nada".
* **Por Qué es Flagrante**:
  * Un sistema Windows activo nunca tiene la carpeta Prefetch vacía. La ausencia total de `.pf` es la prueba forense de que se ejecutó una herramienta de borrado.
  * En el registro de seguridad, el vaciado de eventos genera inmediatamente el **Event ID 1102** (*The audit log was cleared*). Este evento registra explícitamente el nombre del usuario y la hora del borrado, constituyendo una confesión digital irrevocable.

### 4. El Pretexto del "Pack de Optimización" (*FPS Booster Excuse*)
* **Comportamiento**: Justificar que servicios vitales (`DPS`, `PcaSvc`, `SysMain`, `EventLog`) están deshabilitados argumentando: *"Descargué un Windows recortado / un pack de optimización de YouTube para tener más FPS"*.
* **Por Qué es Flagrante**:
  * Los checkers y evaluadores profesionales no aceptan sistemas no auditables.
  * En cualquier liga competitiva estructurada (Echo, Paladin, etc.), la manipulación o detención de los servicios de telemetría y ejecución de Windows se penaliza con el mismo nivel de gravedad que la tenencia de software malicioso confirmado.

### 5. Borrado Simple de Archivos y Vaciar la Papelera
* **Comportamiento**: Seleccionar la carpeta donde estaba el inyector o cheat, presionar `Shift + Delete` o vaciar la Papelera de Reciclaje (`$Recycle.Bin`).
* **Huellas Forenses Generadas**:
  * **$MFT**: El registro del inyector permanece en el archivo maestro de la tabla con el flag `InUse = 0`. Herramientas como `MFTECmd` o `FTK Imager` recuperan el nombre completo, tamaño y marcas de tiempo del archivo eliminado.
  * **$i30 Slack Space**: En el directorio padre, la entrada eliminada queda flotando en el espacio no asignado del árbol B del índice `$i30`.
  * **Event Log / USN**: El identificador de archivo (`FileReferenceNumber`) queda vinculado a las transacciones recientes.

---

## 3. Resumen de Consecuencias Técnicas

| Maniobra Ingenua | Artefacto que la delata | Consecuencia en el Check |
|---|---|---|
| Reinicio intencionado | Event ID 6008 / 41 en System.evtx | Sanción inmediata por evasión de control. |
| Vaciar `Prefetch` | Directorio vacío o sin entradas históricas | Flag de anti-forensics / ban por manipulación. |
| Borrar eventos (`wevtutil cl`) | Event ID 1102 en Security.evtx | Prueba documental de borrado deliberado. |
| Borrado con `Shift+Del` | Registro MFT con `InUse=0` e índice `$i30` | Recuperación del nombre y hash del cheat. |
| Servicios detenidos manualmente | Event ID 7036 en System.evtx y estado `Stopped` | Descalificación técnica por entorno no auditable. |
