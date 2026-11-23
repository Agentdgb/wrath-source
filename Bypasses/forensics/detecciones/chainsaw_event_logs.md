# Detección Forense: Chainsaw (WithSecure Event Log Massacre)

## 1. Identificación y Alcance de la Herramienta
- **Herramienta**: Chainsaw (WithSecure / Countercept).
- **Naturaleza**: Motor forense de búsqueda e inspección rápida de archivos de logs de eventos de Windows (`.evtx`) desarrollado en Rust.
- **Mecanismo**: Ingiere y analiza gigabytes de logs en segundos aplicando un repositorio exhaustivo de reglas **Sigma** y patrones de detección basados en la matriz **MITRE ATT&CK** (Privilege Escalation, Defense Evasion, Persistence, Execution).
- **Canales Auditados**:
  - `Security.evtx`
  - `System.evtx`
  - `Microsoft-Windows-PowerShell%4Operational.evtx`
  - `Microsoft-Windows-Sysmon%4Operational.evtx` (si Sysmon está instalado)
  - `Microsoft-Windows-TaskScheduler%4Operational.evtx`

---

## 2. Reglas Sigma y Patrones de Flag en Screenshares
Chainsaw cruza automáticamente las transacciones y dispara alertas categorizadas (`CRITICAL`, `HIGH`, `MEDIUM`):

1. **Reglas Anti-Forensics / Tampering**:
   - `Event ID 1102` (`Security.evtx`): *"Audit log cleared"*. Clasificación Sigma: **CRITICAL**.
   - `Event ID 104` (`System.evtx`): *"The log-file was cleared"*. Clasificación Sigma: **CRITICAL**.
   - **Saltos en la Secuencia (`EventRecordID Gap`)**: Chainsaw verifica que los identificadores de eventos sean correlativos. Un salto numérico brusco en `EventRecordID` revela que se eliminaron eventos puntuales.
2. **Manipulación y Detención de Servicios**:
   - `Event ID 7036` (`System.evtx`): Detecta cuando un servicio de seguridad o diagnóstico (`DPS`, `PcaSvc`, `WinDefend`, `SysMain`, `DiagTrack`) entra en estado `Stopped`.
3. **Instalación de Drivers y Servicios Sospechosos**:
   - `Event ID 7045` (`System.evtx`) / `Event ID 4697` (`Security.evtx`): Detecta la creación e instalación de nuevos servicios de controladores en modo kernel (típico de mappers y drivers de cheats).
4. **Ejecución y Scripting Anómalo**:
   - `Event ID 4688` (`Security.evtx`): Procesos creados con argumentos sospechosos (`-encodedcommand`, `bypass`, `hidden`, `downloadstring`).
   - `Event ID 4104` (`PowerShell%4Operational.evtx`): Bloques de script de PowerShell que contienen firmas de inyección o reflectores en memoria.
