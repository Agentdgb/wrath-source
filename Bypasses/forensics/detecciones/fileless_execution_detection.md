# Detección Forense: Fileless Execution (PowerShell, CMD, Event Logs, Hayabusa)

## 1. Identificación y Alcance del Vector
- **Vector**: Ejecución "Fileless" (en memoria, sin binarios sueltos en disco) utilizando consolas nativas de Windows (`powershell.exe`, `cmd.exe`), scripts (`.ps1`, `.bat`), WMI o tareas programadas.
- **Objetivo del método**: Evadir artefactos de disco como Prefetch, AmCache y $MFT.
- **Herramientas de Análisis Forense**:
  - `Hayabusa` (Yamato Security): Analizador rápido de logs de eventos de Windows (`.evtx`) basado en reglas Sigma.
  - `Event Viewer` (`winevt\Logs\Microsoft-Windows-PowerShell%4Operational.evtx`, `Security.evtx`).
  - Inspección de `ConsoleHost_history.txt` (PSReadLine).
  - `Regshot` (comparación de snapshots del registro de Windows).

---

## 2. Artefactos y Métodos de Detección

### A. PowerShell ConsoleHost History (`ConsoleHost_history.txt`)
- Ruta: `%APPDATA%\Microsoft\Windows\PowerShell\PSReadLine\ConsoleHost_history.txt`
- Guarda el texto plano de **cada comando** ingresado en la consola interactiva de PowerShell.
- **Revisión en Screenshare**: Los analistas buscan este archivo mediante Everything o PowerShell. Si el usuario ejecutó comandos de inyección, descarga de scripts o limpieza, la línea exacta queda registrada.

### B. Windows Event Logs para PowerShell
1. **Event ID 4104 (Script Block Logging)**:
   - Registra el bloque de código completo ejecutado por el motor de PowerShell.
   - **Crucial**: Registra el código *después* de que se desofusca en memoria. Aunque el script use `Invoke-Obfuscation`, `[System.Convert]::FromBase64String` o XOR, el Evento 4104 registra la carga desofuscada final.
2. **Event ID 4103 (Module Logging)**:
   - Registra nombres de módulos, funciones de pipeline y parámetros ejecutados.
3. **Event ID 600 (Provider Lifecycle)**:
   - Registra la inicialización y proveedores del motor de PowerShell, delatando ejecuciones de PowerShell en segundo plano.

### C. Hayabusa y Process Creation Logging (CMD / PowerShell)
- Hayabusa analiza en segundos todos los `.evtx` de `C:\Windows\System32\winevt\Logs` contra cientos de reglas de detección de amenazas.
- Si el registro de creación de procesos con línea de comandos (Event ID 4688 en `Security.evtx` o Sysmon Event ID 1) está activo, Hayabusa reconstruye la línea de comandos exacta ejecutada por `cmd.exe` o `powershell.exe`.

### D. AMSI (Antimalware Scan Interface)
- Interfaz del sistema operativo (`amsi.dll`) que pasa los buffers de memoria de comandos de PowerShell y scripts a Windows Defender o al antivirus antes de su ejecución (`AmsiScanBuffer`).

### E. Tareas Programadas y Claves de Persistencia
- Tareas creadas vía `schtasks`: auditadas con `schtasks /query /fo LIST /v`.
- Claves de ejecución automática: `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` y `HKLM\...\Run` monitoreadas con Regshot.

---

## 3. Heurísticas y Reglas de Flag en Screenshares
- **Flag 1 — `ConsoleHost_history.txt` Borrado o Vacío**: Si un usuario tiene eventos de PowerShell en el visor de eventos pero su historial de PSReadLine no existe o tiene 0 bytes, se clasifica como **Tampering Directo (Ban)**.
- **Flag 2 — Eventos 4104 con Scripts Sospechosos**: Registros de scripts de memoria, inyectores web o descargadores `IEX (New-Object Net.WebClient).DownloadString`.
- **Flag 3 — Detecciones en Hayabusa**: Hayabusa marca con nivel `high` o `critical` comandos con palabras clave como `bypass`, `hidden`, `amsi`, `downloadstring`, o scripts de limpieza.
- **Flag 4 — Event ID 1102 en Security.evtx**: Registro de auditoría vaciado deliberadamente.
