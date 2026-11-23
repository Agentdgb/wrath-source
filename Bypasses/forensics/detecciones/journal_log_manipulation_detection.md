# Detección Forense: Journal Bypass y Manipulación de Event Logs

## 1. Identificación y Alcance del Procedimiento
- **Vector**: Detección de intentos de borrado o alteración de registros del sistema de archivos (USN Journal), registros de instalación de controladores (`setupapi.dev.log`) y registros de eventos de Windows (`.evtx`).
- **Objetivo**: Detectar si el usuario ejecutó scripts "cleaner", limpiadores de historial o alteró los mecanismos de auditoría del sistema operativo antes o durante la revisión.

---

## 2. Artefactos y Métodos de Detección

### A. Detección de Manipulación en `setupapi.dev.log`
- Ubicación: `%SystemRoot%\INF\setupapi.dev.log` y log de eventos:
  `Applications and Services Logs > Microsoft > Windows > SetupAPI > Dev.log`
- **Firma Crítica**:
  - `Reason = Data Overwrite`: Señala que se sobreescribió un driver o dispositivo de forma no estándar (e.g., inyección de drivers vulnerables para bypass).
  - Los eventos legítimos de hardware figuran como `Device Install (Hardware initiated)`.
  - El registro incluye el SID del usuario que inició la instalación del driver.

### B. Alteración de Claves de Registro de EventLog
- Clave: `HKLM\SYSTEM\CurrentControlSet\Services\EventLog`
- **Técnicas de Bypass Auditadas**:
  - `File`: Redirección del archivo de log a una ruta alternativa o nula.
  - `MaxSize`, `Retention`, `Start`: Reducción del tamaño máximo o deshabilitación del servicio.
- **Monitoreo con Sysmon**:
  - `Event ID 12`: Creación de claves o valores en `Services\EventLog`.
  - `Event ID 13`: Modificación del valor del registro.

### C. Detección de Vaciado de Logs de Eventos (Event Viewer)
- **Event ID 1102 en `Security.evtx`**:
  - `"The audit log was cleared"`: Se dispara automáticamente en el instante exacto en que alguien vacía el log de seguridad. Registra el nombre de la cuenta y el SID del usuario ejecutor.
- **Event ID 104 en `System.evtx`**:
  - `"The log-file was cleared"`: Generado al vaciar logs de sistema o de aplicaciones.
- **Monitoreo de Procesos de Limpieza**:
  - Procesos ejecutando `wevtutil.exe` con los argumentos `cl` (clear) o `sl` (set-log).
  - Comandos de PowerShell ejecutando `Clear-EventLog`.

### D. Monitoreo del USN Journal (NTFS Change Journal)
- **Comando de Inspección**:
  ```cmd
  fsutil usn queryjournal C:
  ```
- **Firmas de Manipulación**:
  - Diario USN vacío o con valores iniciales (`Usn = 0x0` o `LowestValidUsn` recién inicializado) en un equipo con días de actividad.
  - Desconexión del diario o vaciado deliberado con `fsutil usn deletejournal`.

### E. Eventos de Creación de Procesos y Servicios
- `Event ID 4688` (Security): Creación de nuevos procesos con su línea de comandos.
- `Event ID 4697` (Security): *"A service was installed in the system"*. Registra el nombre del servicio, ruta del archivo binario y cuenta de servicio (delata la instalación de drivers `.sys` de cheats).
